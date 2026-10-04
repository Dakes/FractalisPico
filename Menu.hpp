#ifndef MENU_H
#define MENU_H

#include <cstdint>

namespace pimoroni { class PicoGraphics; }

/**
 * The menu: pages of rows on a panel over the left part of the screen, the image stays visible on the right, so
 * changes of the look show right away. Four buttons navigate it: X up, Y down, A OK (held: the second function of
 * some rows), B back (held: close).
 *
 * Drawn in strips like everything else: prepare() lays out a frame, draw() then draws the part of it in each strip.
 */
namespace menu {

struct Color { uint8_t r, g, b; };

// 16 x 16 pixels, '#' = set, '+' = half transparent, anything else = transparent
struct Icon { const char* rows[16]; };

struct Box { int x, y, w, h; };

enum class Kind : uint8_t {
    PAGE,    // opens another page
    SWITCH,  // on/off, A toggles it
    CHOICE,  // one of several options: A opens the list of them. Two options: A toggles.
    ACTION,  // A runs it
    INFO,    // only shows a value
};

struct Page;

/**
 * A row of a page. All callbacks get the row's param, so one function can serve several rows (e.g. the view
 * slots). Build them with the functions below, e.g. choice(...).live().decorated(...).
 */
struct Item {
    Kind kind = Kind::INFO;
    // CHOICE: the option under the cursor applies right away while scrolling through the list. Otherwise on A (for
    // the ones that recalculate the view).
    bool live_preview = false;
    // ACTION: the menu closes afterwards
    bool closes_menu = false;
    uint8_t height = 0;  // 0 = normal row height
    const char* label = nullptr;
    const char* help = nullptr;  // shown at the bottom of the panel while the row is selected
    int param = 0;
    const Page* page = nullptr;  // PAGE
    const Icon* icon = nullptr;
    int (*get)(int param) = nullptr;  // SWITCH: 0/1, CHOICE: the selected option
    void (*set)(int param, int value) = nullptr;
    int (*count)(int param) = nullptr;  // CHOICE: number of options
    // CHOICE: the name of an option. Other rows: their value (option -1). Returns buffer or a constant text.
    const char* (*text)(int param, int option, char* buffer, int size) = nullptr;
    // Instead of label
    const char* (*label_text)(int param, char* buffer, int size) = nullptr;
    void (*run)(int param) = nullptr;   // ACTION
    void (*hold)(int param) = nullptr;  // A held, any kind
    bool (*enabled)(int param) = nullptr;  // greyed out if false
    // Draws a picture of the value or option into the box (decor_width wide, right of the text), e.g. a palette
    // swatch. Called for every strip the box touches, with the drawing functions below. decor_width 0: it draws
    // the whole row instead of the label and value, option is then 1 if the row is selected.
    void (*decor)(int param, int option, const Box& box) = nullptr;
    uint8_t decor_width = 0;

    constexpr Item live() const { Item i = *this; i.live_preview = true; return i; }
    constexpr Item closing() const { Item i = *this; i.closes_menu = true; return i; }
    constexpr Item with_param(int p) const { Item i = *this; i.param = p; return i; }
    constexpr Item with_icon(const Icon& c) const { Item i = *this; i.icon = &c; return i; }
    constexpr Item with_height(int h) const { Item i = *this; i.height = static_cast<uint8_t>(h); return i; }
    constexpr Item with_value(const char* (*t)(int, int, char*, int)) const { Item i = *this; i.text = t; return i; }
    constexpr Item with_label(const char* (*t)(int, char*, int)) const { Item i = *this; i.label_text = t; return i; }
    constexpr Item with_hold(void (*h)(int)) const { Item i = *this; i.hold = h; return i; }
    constexpr Item when(bool (*e)(int)) const { Item i = *this; i.enabled = e; return i; }
    constexpr Item decorated(void (*d)(int, int, const Box&), int width) const {
        Item i = *this;
        i.decor = d;
        i.decor_width = static_cast<uint8_t>(width);
        return i;
    }
    constexpr Item drawn_by(void (*d)(int, int, const Box&)) const { return decorated(d, 0); }
};

struct Page {
    const char* title;
    const Icon* icon;
    Color color;  // accent color of the page and its rows, the LED shows it as well
    const Item* items;
    int count;
};

constexpr Item page(const char* label, const Page& p, const char* help) {
    Item i;
    i.kind = Kind::PAGE;
    i.label = label;
    i.help = help;
    i.page = &p;
    i.icon = p.icon;
    return i;
}

constexpr Item toggle(const char* label, int (*get)(int), void (*set)(int, int), const char* help) {
    Item i;
    i.kind = Kind::SWITCH;
    i.label = label;
    i.help = help;
    i.get = get;
    i.set = set;
    return i;
}

constexpr Item choice(const char* label, int (*get)(int), void (*set)(int, int), int (*count)(int),
                      const char* (*text)(int, int, char*, int), const char* help) {
    Item i;
    i.kind = Kind::CHOICE;
    i.label = label;
    i.help = help;
    i.get = get;
    i.set = set;
    i.count = count;
    i.text = text;
    return i;
}

constexpr Item action(const char* label, void (*run)(int), const char* help) {
    Item i;
    i.kind = Kind::ACTION;
    i.label = label;
    i.help = help;
    i.run = run;
    return i;
}

constexpr Item info(const char* label, const char* (*text)(int, int, char*, int), const char* help) {
    Item i;
    i.kind = Kind::INFO;
    i.label = label;
    i.help = help;
    i.text = text;
    return i;
}

// The top page, shown when the menu opens the first time
void set_root(const Page& root);
bool is_open();
// Opens where it was closed the last time
void open();
void close();
void up();
void down();
void ok();
void hold();
void back();
// Accent color of the current page
Color color();

// Screen size the layout is made for
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
// The panel covers the screen left of this
constexpr int PANEL_W = 196;

// Text colors of the menu
constexpr Color TEXT = {235, 236, 242};
constexpr Color SOFT_TEXT = {150, 152, 168};

// Lays out the next frame, call before it is drawn
void prepare(pimoroni::PicoGraphics& g);
// Sets the strip of rows that draw() and the drawing functions below draw into (display byte order, SCREEN_W
// pixels per row). The whole screen is the clip area again.
void begin_strip(pimoroni::PicoGraphics& g, uint16_t* strip, int first_row, int rows);
// Draws the menu into the strip
void draw();

/**
 * Drawing into the current strip, for draw() and the decor callbacks. Everything is clipped to the strip and the
 * area set with set_clip().
 */
void set_clip(const Box& box);
// True if the box touches the strip, nothing to draw otherwise
bool visible(const Box& box);
void fill(const Box& box, Color c);
// alpha 0-256
void blend(const Box& box, Color c, int alpha);
// Rounded rectangle, anti-aliased edges
void round_rect(const Box& box, int radius, Color c, int alpha = 256);
void disc(float cx, float cy, float radius, Color c, int alpha = 256);
void ring(float cx, float cy, float radius, float width, Color c, int alpha = 256);
void icon(const Icon& icon, int x, int y, Color c);
// One color through a mask of width x height alpha values, 4 bits each (0-15, the low half of a byte first), row
// by row. width must be even.
void mask(int x, int y, int width, int height, const uint8_t* alpha, Color c);
// Pixels in display byte order, width x height, row by row
void image(const uint16_t* pixels, int x, int y, int width, int height);
// One color per column (RGB565, native byte order), stretched over the box height
void columns(const Box& box, const uint16_t* colors);
// Small = font6, otherwise font8. wrap: line width in pixels, 0 = no wrap. lines: lines it may take (for clipping).
void text(const char* s, int x, int y, Color c, bool small = false, int wrap = 0, int lines = 1);
int text_width(const char* s, bool small = false);

}  // namespace menu

#endif // MENU_H
