#include "Menu.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace menu {

namespace {

// Layout, top to bottom
constexpr int HINT_TOP_Y = 3;      // the hint of A
constexpr int TITLE_Y = 17;        // page icon and title
constexpr int RULE_Y = 39;
constexpr int ROWS_Y = 43;
constexpr int ROWS_BOTTOM = 197;
constexpr int HELP_Y = 202;        // two lines
constexpr int HINT_BOTTOM_Y = SCREEN_H - 13;  // the hint of B
constexpr int ROW_H = 24;
constexpr int ROW_X = 5;           // the selection box
constexpr int ROW_W = PANEL_W - 2 * ROW_X - 2;
constexpr int TEXT_X = 13;
constexpr int VALUE_RIGHT = PANEL_W - 13;
constexpr int SWITCH_W = 22, SWITCH_H = 12;
// Rows shown at once at most (partly visible ones included)
constexpr int MAX_ROWS = (ROWS_BOTTOM - ROWS_Y) / 16 + 2;

constexpr Color DISABLED_TEXT = {92, 94, 106};
constexpr Color BLACK = {0, 0, 0};
constexpr Color WHITE = {255, 255, 255};
constexpr Color SWITCH_OFF = {72, 74, 88};
// Added to the darkened image behind the panel, RGB565: a very dark blue
constexpr uint16_t PANEL_TINT = (1 << 11) | (2 << 5) | 4;

const Icon CHEVRON = {{
    "##",
    ".##",
    "..##",
    "...##",
    "..##",
    ".##",
    "##",
}};
const Icon ARROW_UP = {{
    "...#",
    "..###",
    ".#####",
    "#######",
}};
const Icon ARROW_DOWN = {{
    "#######",
    ".#####",
    "..###",
    "...#",
}};

struct Level {
    const Page* page;
    const Item* list;  // CHOICE whose options are shown, nullptr: the items of the page
    int cursor;
    int scroll;  // pixels
};
constexpr int MAX_DEPTH = 6;
Level stack[MAX_DEPTH];
int depth = 0;  // 0: never opened
bool shown = false;
const Page* root = nullptr;

// The strip being drawn
struct Target {
    pimoroni::PicoGraphics* g = nullptr;
    uint16_t* pixels = nullptr;
    int first_row = 0, rows = 0;
    Box clip = {0, 0, SCREEN_W, SCREEN_H};
} target;

// The laid out frame
struct Row {
    int y, h;
    const Item* item;  // the row of the page, or the CHOICE of a list
    int option;        // list rows: the option. Page rows: the selected option of a CHOICE, else -1
    bool list_row;
    bool selected, enabled, checked, on;
    char label[40];
    char value[40];
};
struct Frame {
    Row rows[MAX_ROWS];
    int count;
    Color color;
    const Icon* icon;
    const char* title;
    const char* help;
    int content_h, scroll;
} frame;

Level& top() { return stack[depth - 1]; }

int rows_in(const Level& l) {
    return l.list ? l.list->count(l.list->param) : l.page->count;
}

int height_of(const Level& l, int i) {
    if (l.list) return ROW_H;
    int h = l.page->items[i].height;
    return h ? h : ROW_H;
}

// Scrolls so that the cursor row is visible, with a bit of the rows around it
void keep_visible(Level& l) {
    int n = rows_in(l);
    if (n == 0) {
        l.cursor = l.scroll = 0;
        return;
    }
    l.cursor = std::min(l.cursor, n - 1);
    int y = 0, total = 0;
    for (int i = 0; i < n; ++i) {
        if (i == l.cursor) y = total;
        total += height_of(l, i);
    }
    const int area = ROWS_BOTTOM - ROWS_Y;
    const int h = height_of(l, l.cursor);
    const int margin = std::min(ROW_H / 2, std::max(0, (area - h) / 2));
    if (y - margin < l.scroll) l.scroll = y - margin;
    if (y + h + margin > l.scroll + area) l.scroll = y + h + margin - area;
    l.scroll = std::max(0, std::min(l.scroll, total - area));
}

void push(const Level& l) {
    if (depth >= MAX_DEPTH) return;
    stack[depth++] = l;
    keep_visible(top());
}

void move(int delta) {
    if (!shown || depth == 0) return;
    Level& l = top();
    int n = rows_in(l);
    if (n == 0) return;
    l.cursor = (l.cursor + delta + n) % n;
    if (l.list && l.list->live_preview) l.list->set(l.list->param, l.cursor);
    keep_visible(l);
}

uint16_t pack(Color c) {
    return static_cast<uint16_t>(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

uint16_t swap_bytes(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

Color mix(Color a, Color b, float f) {
    auto m = [f](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

// Rows of the box inside the strip and the clip area
bool clip_rows(int y0, int y1, int& from, int& to) {
    from = std::max({y0, target.first_row, target.clip.y});
    to = std::min({y1, target.first_row + target.rows, target.clip.y + target.clip.h});
    return from < to;
}

uint16_t* pixel(int x, int y) {
    return target.pixels + (y - target.first_row) * SCREEN_W + x;
}

void blend_pixel(uint16_t* p, uint16_t c, int alpha) {
    if (alpha >= 256) {
        *p = swap_bytes(c);
        return;
    }
    const int v = swap_bytes(*p);
    int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    r += ((static_cast<int>(c >> 11) - r) * alpha) >> 8;
    g += ((static_cast<int>((c >> 5) & 63) - g) * alpha) >> 8;
    b += ((static_cast<int>(c & 31) - b) * alpha) >> 8;
    *p = swap_bytes(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

// A row of pixels from x0 to x1 (fractional: the edge pixels are blended by their coverage)
void span(int y, float x0, float x1, uint16_t c, int alpha) {
    const int left = std::max(static_cast<int>(std::floor(x0)), target.clip.x);
    const int right = std::min(static_cast<int>(std::ceil(x1)), target.clip.x + target.clip.w);
    uint16_t* p = pixel(left, y);
    for (int x = left; x < right; ++x, ++p) {
        float cover = std::min(x + 1.0f, x1) - std::max(static_cast<float>(x), x0);
        if (cover >= 0.999f)
            blend_pixel(p, c, alpha);
        else if (cover > 0.0f)
            blend_pixel(p, c, static_cast<int>(alpha * cover));
    }
}

void draw_glyph(const Icon& g, int x, int y, int rows, Color c) {
    int from, to;
    if (!clip_rows(y, y + rows, from, to)) return;
    const uint16_t c565 = pack(c);
    for (int py = from; py < to; ++py) {
        const char* row = g.rows[py - y];
        if (!row) continue;
        for (int i = 0; row[i] && i < 16; ++i) {
            int px = x + i;
            if (px < target.clip.x || px >= target.clip.x + target.clip.w) continue;
            if (row[i] == '#')
                blend_pixel(pixel(px, py), c565, 256);
            else if (row[i] == '+')
                blend_pixel(pixel(px, py), c565, 128);
        }
    }
}

// The darkened image behind the panel
void draw_panel_background() {
    int from, to;
    if (!clip_rows(0, SCREEN_H, from, to)) return;
    for (int y = from; y < to; ++y) {
        uint16_t* p = pixel(0, y);
        for (int x = 0; x < PANEL_W; ++x, ++p) {
            uint16_t v = swap_bytes(*p);
            // 3/16 of the brightness
            v = static_cast<uint16_t>(((v >> 3) & 0x18E3) + ((v >> 4) & 0x0861) + PANEL_TINT);
            *p = swap_bytes(v);
        }
    }
    // A thin edge in the page color
    blend({PANEL_W - 1, 0, 1, SCREEN_H}, frame.color, 140);
}

// The letters of the keys, 5 x 7
const Icon KEY_A = {{".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}};
const Icon KEY_B = {{"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}};
const Icon KEY_X = {{"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}};
const Icon KEY_Y = {{"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}};

// A key as a small light square with its letter, then what it does
void draw_hint(const Icon& key, const char* what, int x, int y) {
    round_rect({x, y, 11, 11}, 2, {225, 226, 232});
    draw_glyph(key, x + 3, y + 2, 7, BLACK);
    if (what) text(what, x + 15, y + 3, SOFT_TEXT, true);
}

// The keys of the image side: X up, Y down, on a dark backdrop
void draw_arrow_hint(const Icon& key, const Icon& arrow, int y) {
    const int x = SCREEN_W - 28;
    round_rect({x - 3, y - 3, 28, 17}, 4, {12, 12, 22}, 170);
    draw_hint(key, nullptr, x, y);
    draw_glyph(arrow, x + 15, y + 4, 4, TEXT);
}

void draw_switch(int x, int y, bool on, Color color) {
    round_rect({x, y, SWITCH_W, SWITCH_H}, SWITCH_H / 2, on ? color : SWITCH_OFF);
    const float r = SWITCH_H / 2.0f - 2.0f;
    disc(on ? x + SWITCH_W - SWITCH_H / 2.0f : x + SWITCH_H / 2.0f, y + SWITCH_H / 2.0f, r, on ? WHITE : SOFT_TEXT);
}

void draw_row(const Row& r) {
    const Box box = {ROW_X, r.y, ROW_W, r.h};
    if (!visible(box)) return;
    const Item& item = *r.item;
    if (r.selected) {
        round_rect({box.x, box.y + 1, box.w, box.h - 2}, 5, frame.color, 70);
        round_rect({box.x, box.y + 5, 3, box.h - 10}, 1, frame.color);
    }
    if (!r.list_row && item.decor && item.decor_width == 0) {
        item.decor(item.param, r.selected, {box.x + 6, box.y, box.w - 6, box.h});
        return;
    }
    const Color label_color = r.enabled ? TEXT : DISABLED_TEXT;
    const int cy = r.y + r.h / 2;
    int x = TEXT_X;

    if (r.list_row) {
        // Radio button
        ring(x + 4.5f, cy, 4.5f, 1.2f, r.checked ? frame.color : SOFT_TEXT);
        if (r.checked) disc(x + 4.5f, cy, 2.2f, WHITE);
        x += 15;
    } else if (item.icon) {
        draw_glyph(*item.icon, x - 1, cy - 8, 16, item.kind == Kind::PAGE ? item.page->color : label_color);
        x += 21;
    }
    text(r.label, x, cy - 4, label_color);

    int right = VALUE_RIGHT;
    const bool chevron = !r.list_row && (item.kind == Kind::PAGE
                                         || (item.kind == Kind::CHOICE && item.count(item.param) > 2));
    if (chevron) {
        draw_glyph(CHEVRON, right - 4, cy - 3, 7, r.selected ? TEXT : SOFT_TEXT);
        right -= 11;
    }
    if (!r.list_row && item.kind == Kind::SWITCH) {
        draw_switch(right - SWITCH_W, cy - SWITCH_H / 2, r.on, frame.color);
        right -= SWITCH_W + 6;
    }
    if (r.value[0]) {
        const int w = text_width(r.value);
        Color value_color = !r.enabled ? DISABLED_TEXT : r.selected ? WHITE : mix(frame.color, WHITE, 0.55f);
        text(r.value, right - w, cy - 4, value_color);
        right -= w + 6;
    }
    if (item.decor && (r.list_row || item.kind != Kind::PAGE)) {
        int w = item.decor_width;
        item.decor(item.param, r.option, {right - w, cy - 6, w, 12});
    }
}

void draw_scrollbar() {
    const int area = ROWS_BOTTOM - ROWS_Y;
    if (frame.content_h <= area) return;
    const int x = PANEL_W - 5;
    blend({x, ROWS_Y, 2, area}, WHITE, 30);
    int thumb = std::max(12, area * area / frame.content_h);
    int y = ROWS_Y + (area - thumb) * frame.scroll / (frame.content_h - area);
    round_rect({x, y, 2, thumb}, 1, frame.color, 200);
}

void fill_row(Row& r, const Level& l, int i, int y) {
    r.y = y;
    r.h = height_of(l, i);
    r.selected = i == l.cursor;
    r.checked = r.on = false;
    r.value[0] = '\0';
    auto put = [](char* out, int size, const char* s) {
        if (s != out) snprintf(out, size, "%s", s ? s : "");
    };
    if (l.list) {
        const Item& c = *l.list;
        r.item = &c;
        r.list_row = true;
        r.option = i;
        r.enabled = true;
        r.checked = c.get(c.param) == i;
        put(r.label, sizeof(r.label), c.text(c.param, i, r.label, sizeof(r.label)));
        return;
    }
    const Item& item = l.page->items[i];
    r.item = &item;
    r.list_row = false;
    r.option = -1;
    r.enabled = !item.enabled || item.enabled(item.param);
    put(r.label, sizeof(r.label), item.label_text ? item.label_text(item.param, r.label, sizeof(r.label)) : item.label);
    switch (item.kind) {
        case Kind::SWITCH:
            r.on = item.get(item.param) != 0;
            break;
        case Kind::CHOICE:
            r.option = item.get(item.param);
            put(r.value, sizeof(r.value), item.text(item.param, r.option, r.value, sizeof(r.value)));
            break;
        default:
            if (item.text) put(r.value, sizeof(r.value), item.text(item.param, -1, r.value, sizeof(r.value)));
            break;
    }
}

}  // namespace

void set_root(const Page& page) {
    root = &page;
    depth = 0;
}

bool is_open() {
    return shown;
}

void open() {
    if (!root) return;
    if (depth == 0) {
        stack[0] = {root, nullptr, 0, 0};
        depth = 1;
    }
    shown = true;
}

void close() {
    shown = false;
}

void up() {
    move(-1);
}

void down() {
    move(1);
}

void ok() {
    if (!shown || depth == 0) return;
    Level& l = top();
    if (l.list) {
        const Item& c = *l.list;
        if (c.get(c.param) != l.cursor) c.set(c.param, l.cursor);
        depth--;
        return;
    }
    if (rows_in(l) == 0) return;
    const Item& item = l.page->items[l.cursor];
    if (item.enabled && !item.enabled(item.param)) return;
    switch (item.kind) {
        case Kind::PAGE:
            push({item.page, nullptr, 0, 0});
            break;
        case Kind::SWITCH:
            item.set(item.param, !item.get(item.param));
            break;
        case Kind::CHOICE:
            if (item.count(item.param) == 2)
                item.set(item.param, 1 - item.get(item.param));
            else
                push({l.page, &item, item.get(item.param), 0});
            break;
        case Kind::ACTION:
            if (item.run) item.run(item.param);
            if (item.closes_menu) close();
            break;
        case Kind::INFO:
            break;
    }
}

void hold() {
    if (!shown || depth == 0) return;
    Level& l = top();
    if (l.list || rows_in(l) == 0) return;
    const Item& item = l.page->items[l.cursor];
    if (item.hold && (!item.enabled || item.enabled(item.param))) item.hold(item.param);
}

void back() {
    if (!shown) return;
    if (depth > 1)
        depth--;
    else
        close();
}

Color color() {
    return depth > 0 ? top().page->color : Color{255, 255, 255};
}

void prepare(pimoroni::PicoGraphics& g) {
    target.g = &g;
    frame.count = 0;
    if (!shown || depth == 0) return;
    Level& l = top();
    keep_visible(l);
    frame.color = l.page->color;
    frame.icon = l.page->icon;
    frame.title = l.list ? l.list->label : l.page->title;
    const int n = rows_in(l);
    frame.help = l.list ? l.list->help : n > 0 ? l.page->items[l.cursor].help : nullptr;
    frame.scroll = l.scroll;
    int y = ROWS_Y - l.scroll;
    for (int i = 0; i < n; ++i) {
        int h = height_of(l, i);
        if (y + h > ROWS_Y && y < ROWS_BOTTOM && frame.count < MAX_ROWS) fill_row(frame.rows[frame.count++], l, i, y);
        y += h;
    }
    frame.content_h = y + l.scroll - ROWS_Y;
}

void begin_strip(pimoroni::PicoGraphics& g, uint16_t* strip, int first_row, int rows) {
    target.g = &g;
    target.pixels = strip;
    target.first_row = first_row;
    target.rows = rows;
    target.clip = {0, 0, SCREEN_W, SCREEN_H};
}

void draw() {
    if (!shown || depth == 0 || !target.g) return;
    pimoroni::PicoGraphics& g = *target.g;
    const int first_row = target.first_row, rows = target.rows;

    draw_panel_background();
    draw_hint(KEY_A, "OK", 5, HINT_TOP_Y);
    draw_hint(KEY_B, "Back   hold: close", 5, HINT_BOTTOM_Y);
    draw_arrow_hint(KEY_X, ARROW_UP, HINT_TOP_Y);
    draw_arrow_hint(KEY_Y, ARROW_DOWN, HINT_BOTTOM_Y);

    // Header
    if (visible({0, TITLE_Y, PANEL_W, RULE_Y + 1 - TITLE_Y})) {
        if (frame.icon) draw_glyph(*frame.icon, TEXT_X - 1, TITLE_Y + 2, 16, frame.color);
        g.set_font(&font14_outline);
        g.set_pen(255, 255, 255);
        g.clip = pimoroni::Rect(0, first_row, SCREEN_W, rows);
        g.text(frame.title, pimoroni::Point(TEXT_X + 21, TITLE_Y + 3), SCREEN_W, 1);
        blend({TEXT_X - 2, RULE_Y, PANEL_W - 2 * TEXT_X + 4, 1}, frame.color, 110);
    }

    set_clip({0, ROWS_Y, PANEL_W, ROWS_BOTTOM - ROWS_Y});
    for (int i = 0; i < frame.count; ++i) draw_row(frame.rows[i]);
    set_clip({0, 0, SCREEN_W, SCREEN_H});
    draw_scrollbar();

    if (frame.help) text(frame.help, TEXT_X, HELP_Y, SOFT_TEXT, false, PANEL_W - 2 * TEXT_X + 2, 2);
}

void set_clip(const Box& box) {
    target.clip = box;
}

bool visible(const Box& box) {
    int from, to;
    return box.w > 0 && clip_rows(box.y, box.y + box.h, from, to);
}

void fill(const Box& box, Color c) {
    blend(box, c, 256);
}

void blend(const Box& box, Color c, int alpha) {
    int from, to;
    if (!clip_rows(box.y, box.y + box.h, from, to)) return;
    const int x0 = std::max(box.x, target.clip.x);
    const int x1 = std::min(box.x + box.w, target.clip.x + target.clip.w);
    const uint16_t c565 = pack(c);
    for (int y = from; y < to; ++y) {
        uint16_t* p = pixel(x0, y);
        for (int x = x0; x < x1; ++x) blend_pixel(p++, c565, alpha);
    }
}

void round_rect(const Box& box, int radius, Color c, int alpha) {
    int from, to;
    if (!clip_rows(box.y, box.y + box.h, from, to)) return;
    const float r = std::min({static_cast<float>(radius), box.w / 2.0f, box.h / 2.0f});
    const uint16_t c565 = pack(c);
    for (int y = from; y < to; ++y) {
        const float cy = y + 0.5f;
        float d = 0.0f;
        if (cy < box.y + r)
            d = box.y + r - cy;
        else if (cy > box.y + box.h - r)
            d = cy - (box.y + box.h - r);
        const float inset = r - std::sqrt(std::max(0.0f, r * r - d * d));
        span(y, box.x + inset, box.x + box.w - inset, c565, alpha);
    }
}

void disc(float cx, float cy, float radius, Color c, int alpha) {
    int from, to;
    if (!clip_rows(static_cast<int>(std::floor(cy - radius)), static_cast<int>(std::ceil(cy + radius)), from, to))
        return;
    const uint16_t c565 = pack(c);
    for (int y = from; y < to; ++y) {
        const float d = y + 0.5f - cy;
        const float half_sq = radius * radius - d * d;
        if (half_sq <= 0.0f) continue;
        const float half = std::sqrt(half_sq);
        span(y, cx - half, cx + half, c565, alpha);
    }
}

void ring(float cx, float cy, float radius, float width, Color c, int alpha) {
    int from, to;
    if (!clip_rows(static_cast<int>(std::floor(cy - radius)), static_cast<int>(std::ceil(cy + radius)), from, to))
        return;
    const uint16_t c565 = pack(c);
    const float inner = radius - width;
    for (int y = from; y < to; ++y) {
        const float d = y + 0.5f - cy;
        const float outer_sq = radius * radius - d * d;
        if (outer_sq <= 0.0f) continue;
        const float outer = std::sqrt(outer_sq);
        const float inner_sq = inner * inner - d * d;
        if (inner_sq <= 0.0f) {
            span(y, cx - outer, cx + outer, c565, alpha);
            continue;
        }
        const float in = std::sqrt(inner_sq);
        span(y, cx - outer, cx - in, c565, alpha);
        span(y, cx + in, cx + outer, c565, alpha);
    }
}

void icon(const Icon& icon, int x, int y, Color c) {
    draw_glyph(icon, x, y, 16, c);
}

void image(const uint16_t* pixels, int x, int y, int width, int height) {
    int from, to;
    if (!clip_rows(y, y + height, from, to)) return;
    const int x0 = std::max(x, target.clip.x);
    const int x1 = std::min(x + width, target.clip.x + target.clip.w);
    for (int py = from; py < to; ++py) {
        const uint16_t* src = pixels + (py - y) * width + (x0 - x);
        uint16_t* dst = pixel(x0, py);
        for (int px = x0; px < x1; ++px) *dst++ = *src++;
    }
}

void columns(const Box& box, const uint16_t* colors) {
    int from, to;
    if (!clip_rows(box.y, box.y + box.h, from, to)) return;
    const int x0 = std::max(box.x, target.clip.x);
    const int x1 = std::min(box.x + box.w, target.clip.x + target.clip.w);
    for (int y = from; y < to; ++y) {
        uint16_t* p = pixel(x0, y);
        for (int x = x0; x < x1; ++x) *p++ = swap_bytes(colors[x - box.x]);
    }
}

void text(const char* s, int x, int y, Color c, bool small, int wrap, int lines) {
    const bitmap::font_t* font = small ? &font6 : &font8;
    int from, to;
    if (!target.g || !clip_rows(y, y + lines * (font->height + 1), from, to)) return;
    pimoroni::PicoGraphics& g = *target.g;
    g.clip = pimoroni::Rect(target.clip.x, from, target.clip.w, to - from);
    g.set_font(font);
    g.set_pen(c.r, c.g, c.b);
    g.text(s, pimoroni::Point(x, y), wrap > 0 ? wrap : SCREEN_W * 2, 1);
}

int text_width(const char* s, bool small) {
    if (!target.g) return 0;
    target.g->set_font(small ? &font6 : &font8);
    return target.g->measure_text(s, 1);
}

}  // namespace menu
