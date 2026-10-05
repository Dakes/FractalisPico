#include "Ui.hpp"
#include "Menu.hpp"
#include "BulbNamer.hpp"
#include "MinibrotFinder.hpp"
#include "UsbDrive.hpp"
#include "globals.h"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <complex>
#include <cstring>

using namespace pimoroni;
using menu::Box;
using menu::Color;

namespace ui {

volatile bool menu_active = false;
volatile bool quick_shown = false;
FrameStats view_frames = {};

Settings current_settings();
void apply_settings(const Settings& s);

namespace {

constexpr int SCREEN_W = menu::SCREEN_W;
constexpr int SCREEN_H = menu::SCREEN_H;
constexpr float PI = 3.14159265f;

// ---- The look ----

enum class ColorCycle : uint8_t { AUTO_ZOOM, ALWAYS, OFF };
ColorCycle color_cycle = ColorCycle::ALWAYS;
const char* const COLOR_CYCLE_NAMES[] = {"auto zoom", "always", "off"};

// Time for one round through the palette
struct CycleSpeed {
    const char* name;
    float seconds;
};
const CycleSpeed CYCLE_SPEEDS[] = {{"100 s", 100.0f}, {"50 s", 50.0f}, {"25 s", 25.0f},
                                   {"12 s", 12.0f},   {"6 s", 6.0f},   {"3 s", 3.0f}};
constexpr int CYCLE_SPEED_COUNT = sizeof(CYCLE_SPEEDS) / sizeof(CYCLE_SPEEDS[0]);
constexpr int DEFAULT_CYCLE_SPEED = 2;
int cycle_speed = DEFAULT_CYCLE_SPEED;
float color_phase = 0.0f;

// Palette repetitions, relative to the auto contrast. id: what the settings store (older versions had the first
// five ids).
struct Bands {
    float factor;
    const char* name;
    uint8_t id;
};
const Bands BANDS[] = {{0.125f, "0.125x", 6}, {0.25f, "0.25x", 3}, {0.5f, "0.5x", 4}, {1.0f, "1x", 0},
                       {2.0f, "2x", 1},       {4.0f, "4x", 2},     {8.0f, "8x", 5}};
constexpr int BANDS_COUNT = sizeof(BANDS) / sizeof(BANDS[0]);
constexpr int DEFAULT_BANDS = 3;
int bands = DEFAULT_BANDS;

constexpr float LIGHT_ANGLES[] = {-0.75f * PI, -0.25f * PI, 0.25f * PI, 0.75f * PI};
const char* const LIGHT_NAMES[] = {"top left", "top right", "bottom right", "bottom left", "rotating"};
constexpr int LIGHT_COUNT = sizeof(LIGHT_NAMES) / sizeof(LIGHT_NAMES[0]);
constexpr int LIGHT_ROTATING = LIGHT_COUNT - 1;
constexpr float LIGHT_ROTATION_SPEED = 2.0f * PI / 12.0f;  // radians per second: a turn in 12 s
int light = 0;
float light_angle = LIGHT_ANGLES[0];

const char* const TRAP_NAMES[] = {"off", "point", "cross", "ring", "period", "stripes"};
static_assert(sizeof(TRAP_NAMES) / sizeof(TRAP_NAMES[0]) == Fractalis::TRAP_COUNT, "a name for every trap");
const char* const REGION_NAMES[] = {"everywhere", "inside", "outside"};
static_assert(sizeof(REGION_NAMES) / sizeof(REGION_NAMES[0]) == Fractalis::REGION_COUNT, "a name for every region");

// Saved as one byte: the trap in the low 4 bit, where it colors above (0 = everywhere, as older records have it)
uint8_t trap_byte() {
    return static_cast<uint8_t>(fractalis.orbit_trap() | (fractalis.orbit_trap_region() << 4));
}
void update_outlines();
void apply_trap_byte(uint8_t b) {
    fractalis.set_orbit_trap(b & 15, b >> 4);
    update_outlines();
}

// Distance estimation: off, the colors by distance without lines, then outlines from thin to bold
const char* const DISTANCE_NAMES[] = {"off", "no lines", "thin lines", "lines", "bold lines"};
constexpr int DISTANCE_COUNT = sizeof(DISTANCE_NAMES) / sizeof(DISTANCE_NAMES[0]);
static_assert(DISTANCE_COUNT - 1 == palette::Palette::OUTLINE_COUNT, "an option for every outline width");
int distance = 0;

// Only where the outside is colored by distance, not over the colors of an orbit trap
void update_outlines() {
    color_palette.set_outlines(fractalis.distance_colors() ? distance - 1 : 0);
}

void set_distance(int option) {
    distance = std::max(0, std::min(option, DISTANCE_COUNT - 1));
    fractalis.set_distance(distance > 0);
    update_outlines();
}

// The solid color palette: the hue in 24 steps around the color wheel, saturation and brightness in steps of 10 %
constexpr int HUE_STEPS = 24;
const char* const HUE_NAMES[] = {"red",  "orange", "yellow", "lime",   "green",   "mint",
                                 "cyan", "azure",  "blue",   "violet", "magenta", "rose"};
static_assert(sizeof(HUE_NAMES) / sizeof(HUE_NAMES[0]) * 2 == HUE_STEPS, "a name every second hue");
struct SolidColor {
    int hue = 0, saturation = 0, brightness = 10;  // white
};
SolidColor solid;

uint16_t solid_rgb(const SolidColor& c) {
    return palette::hsv(static_cast<float>(c.hue) / HUE_STEPS, c.saturation / 10.0f, c.brightness / 10.0f);
}

void apply_solid_color() {
    palette::set_solid_color(solid_rgb(solid));
    if (color_palette.index() == palette::SOLID) color_palette.select(palette::SOLID);
}

// Every second hue has a name, the ones in between both of their neighbors: "red-orange"
const char* hue_name(int hue, char* buffer, int size) {
    if (hue % 2 == 0) return HUE_NAMES[hue / 2];
    snprintf(buffer, size, "%s-%s", HUE_NAMES[hue / 2], HUE_NAMES[(hue / 2 + 1) % (HUE_STEPS / 2)]);
    return buffer;
}

/**
 * Saved in 16 bit next to the rest of the look, 0 (as older records have it) is off and white: distance estimation
 * in bits 0-2, the solid color above it: hue in bits 3-7, saturation in 8-11, 10 - brightness in 12-15.
 */
uint16_t pack_look(int distance_option, const SolidColor& c) {
    return static_cast<uint16_t>(distance_option | c.hue << 3 | c.saturation << 8 | (10 - c.brightness) << 12);
}
int look_distance(uint16_t look) {
    return std::min(look & 7, DISTANCE_COUNT - 1);
}
SolidColor look_color(uint16_t look) {
    SolidColor c;
    c.hue = std::min((look >> 3) & 31, HUE_STEPS - 1);
    c.saturation = std::min((look >> 8) & 15, 10);
    c.brightness = 10 - std::min(look >> 12, 10);
    return c;
}
uint16_t extra_look() {
    return pack_look(distance, solid);
}
// color: take the solid color as well (only when the look uses it, the picker keeps its color otherwise)
void apply_extra_look(uint16_t look, bool color) {
    set_distance(look_distance(look));
    if (color) {
        solid = look_color(look);
        apply_solid_color();
    }
}

constexpr int SUPERSAMPLING[] = {1, 2, 3, 4, 6, 8};
constexpr int SUPERSAMPLING_COUNT = sizeof(SUPERSAMPLING) / sizeof(SUPERSAMPLING[0]);

// Info overlay
enum Hud : uint8_t { HUD_OFF, HUD_ON, HUD_AUTO };
const char* const HUD_NAMES[] = {"off", "on", "auto"};
constexpr int HUD_COUNT = sizeof(HUD_NAMES) / sizeof(HUD_NAMES[0]);
uint8_t hud = HUD_ON;

void set_bands(int index) {
    bands = std::max(0, std::min(index, BANDS_COUNT - 1));
    color_palette.set_bands(BANDS[bands].factor);
}

void set_bands_id(uint8_t id) {
    int index = DEFAULT_BANDS;
    for (int i = 0; i < BANDS_COUNT; ++i) {
        if (BANDS[i].id == id) index = i;
    }
    set_bands(index);
}

void set_light(int index) {
    light = std::max(0, std::min(index, LIGHT_COUNT - 1));
    if (light != LIGHT_ROTATING) {
        light_angle = LIGHT_ANGLES[light];
        color_palette.set_light(light_angle);
    }
}

void set_auto_zoom(bool on);

// ---- Input, saving, short messages ----

uint32_t last_input_ms = 0;
Settings saved;  // what is in the flash

char toast_text[40];
uint32_t toast_until = 0;

void show_toast(uint32_t duration_ms, const char* format, va_list args) {
    vsnprintf(toast_text, sizeof(toast_text), format, args);
    toast_until = now_ms() + duration_ms;
    state.needs_redraw = true;
}

void toast(const char* format, ...) {
    va_list args;
    va_start(args, format);
    show_toast(TOAST_MS, format, args);
    va_end(args);
}

// For messages with more to read
void long_toast(const char* format, ...) {
    va_list args;
    va_start(args, format);
    show_toast(3 * TOAST_MS, format, args);
    va_end(args);
}

bool toast_active(uint32_t now) {
    return toast_text[0] && static_cast<int32_t>(toast_until - now) > 0;
}

// Short, for the rows of the menu
void format_zoom(char* out, int size, double zoom) {
    snprintf(out, size, "x%.3g", zoom);
}

// Large zooms like x2.1e40
void format_deep_zoom(char* out, int size, double zoom) {
    int exponent = static_cast<int>(std::floor(std::log10(zoom)));
    double mantissa = zoom / std::pow(10.0, exponent);
    if (mantissa >= 9.95) {
        mantissa /= 10;
        exponent++;
    }
    snprintf(out, size, "x%.1fe%d", mantissa, exponent);
}

// ---- Views: the way back, places, saved views ----

struct PastView {
    Coordinate center;
    double zoom;
};
constexpr int HISTORY_SIZE = 8;
PastView history[HISTORY_SIZE];
int history_count = 0;

bool is_current(const Coordinate& center, double zoom) {
    return zoom == state.zoom_factor && memcmp(&center, &state.center, sizeof(Coordinate)) == 0;
}

// Before a jump: the current view, to go back there
void remember_view() {
    if (history_count > 0 && is_current(history[history_count - 1].center, history[history_count - 1].zoom)) return;
    if (history_count == HISTORY_SIZE) {
        memmove(history, history + 1, sizeof(PastView) * (HISTORY_SIZE - 1));
        history_count--;
    }
    history[history_count++] = {state.center, state.zoom_factor};
}

void jump(const Coordinate& center, double zoom) {
    remember_view();
    fractalis.set_view(center, zoom);
}

// ---- Minibrot finder ----

MinibrotFinder finder;
uint32_t search_started_ms = 0;
uint32_t search_seconds = 0;  // shown in the message
int target_period = 0;        // of the minibrot auto zoom dives to
uint32_t where_calculation = 0;  // the view "where am I" describes (see below)
int glow_cell = -1;           // the minibrot glow uses the finder too: the cell it probes, -1 = none
bool where_probing = false;   // and "where am I"

// A search started from the menu, not the glow or "where am I"
bool user_search() {
    return finder.busy() && glow_cell < 0 && !where_probing;
}

// The menu offers x1e10 to x1e70
constexpr int FINDER_DEPTHS = 7;
double finder_depth(int i) {
    return std::pow(10.0, 10 * (i + 1));
}

/**
 * Points outside of the set to search from: the screen center, then from rings around it the escaped pixel closest
 * to the set (highest palette position). Inside the set it can't start.
 */
int search_starts(Coordinate* out) {
    const int w = state.screen_w, h = state.screen_h;
    auto outside = [&](int x, int y) { return state.pixelState[y][x].hasPosition(); };
    int count = 0;
    const bool center_outside = outside(w / 2 - 1, h / 2 - 1) || outside(w / 2, h / 2 - 1)
                             || outside(w / 2 - 1, h / 2) || outside(w / 2, h / 2);
    if (center_outside) out[count++] = state.center;
    constexpr int RINGS = 4;
    constexpr int RING_START[RINGS + 1] = {0, 6, 20, 60, 400};  // distance from the center, in pixels
    struct Best {
        int x = -1, y = -1;
        uint32_t position = 0;
    } best[RINGS];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!outside(x, y)) continue;
            const int dx = 2 * x + 1 - w, dy = 2 * y + 1 - h;  // in half pixels
            const int d2 = (dx * dx + dy * dy) / 4;
            int ring = 0;
            while (ring < RINGS && d2 >= RING_START[ring + 1] * RING_START[ring + 1]) ring++;
            if (ring == RINGS || (ring == 0 && center_outside)) continue;
            uint32_t position = state.pixelState[y][x].position();
            if (best[ring].x < 0 || position > best[ring].position) best[ring] = {x, y, position};
        }
    }
    // The pixel centers, like Fractalis::choose_reference()
    const Fixed half_step(2.0 / state.zoom_factor / w);
    for (int i = 0; i < RINGS && count < MinibrotFinder::MAX_STARTS; ++i) {
        if (best[i].x < 0) continue;
        Coordinate c = state.center;
        c.real += half_step.times(2 * best[i].x + 1 - w);
        c.imag += half_step.times(2 * best[i].y + 1 - h);
        out[count++] = c;
    }
    return count;
}

void find_minibrot(int depth) {
    Coordinate starts[MinibrotFinder::MAX_STARTS];
    const int count = search_starts(starts);
    if (count == 0) {
        toast("Nothing but the set in view");
        show_led_feedback(255, 0, 0, 300);
        return;
    }
    glow_cell = -1;  // the glow scan and "where am I" start over after it
    if (where_probing) {
        where_probing = false;
        where_calculation--;  // not the current one any more
    }
    finder.start(starts, count, finder_depth(depth));
    search_started_ms = now_ms();
    search_seconds = 0;
    toast("Looking for a minibrot...");
}

// Moves there, auto zoom gets it as target
void search_done() {
    const uint32_t ms = now_ms() - search_started_ms;
    if (!finder.found()) {
        printf("No minibrot found after %lu iterations, %lu ms\n", static_cast<unsigned long>(finder.iterations()),
               static_cast<unsigned long>(ms));
        char zoom_text[16];
        format_deep_zoom(zoom_text, sizeof(zoom_text), finder.target());
        long_toast("No minibrot at %s found here", zoom_text);
        show_led_feedback(255, 0, 0, 300);
        return;
    }
    const MinibrotFinder::Result& r = finder.result();
    // Framed like the whole set at zoom 1, centered at -0.5 in its coordinates
    const std::complex<double> offset = -0.5 / r.scale;
    const Coordinate center = {r.nucleus.real + Fixed(offset.real()), r.nucleus.imag + Fixed(offset.imag())};
    remember_view();
    fractalis.move_to(center);
    autoZoom.set_target(state.center, r.zoom());
    target_period = r.period;
    char zoom_text[16];
    format_deep_zoom(zoom_text, sizeof(zoom_text), r.zoom());
    printf("Minibrot of period %d at zoom %.3e (target %.0e) after %lu iterations, %d probes, %lu ms\n", r.period,
           r.zoom(), finder.target(), static_cast<unsigned long>(finder.iterations()), finder.probes(),
           static_cast<unsigned long>(ms));
    long_toast("Minibrot at %s, period %d", zoom_text, r.period);
    show_led_feedback(255, 255, 255, 300);
}

/**
 * Places to go: famous spots and the deep zoom tests (at the limit of the old double-double precision, then a few
 * zoom steps further, where the pixels switch to the scaled perturbation).
 */
struct Place {
    const char* name;
    double zoom;
    const char* real;
    const char* imag;
};
const Place PLACES[] = {
    {"Seahorse valley", 600, "-0.74529", "0.11307"},
    {"Elephant valley", 250, "0.2850", "0.0110"},
    {"Spiral valley", 1500, "-0.0880", "0.6550"},
    {"Minibrot", 90, "-1.7548776662466927", "0"},
    {"Tendrils", 300, "-0.226266648", "1.11617444"},
    {"Spiral", 3e4, "-0.761574", "-0.0847596"},
    {"Seahorse spirals", 1e5, "-0.743643887037151", "0.131825904205330"},
    {"Seahorse dive 1", 1e31, "-0.74363021764435289490180174187036443524444484224292785319222673934875385048",
     "0.13179414728676452841409172905993588426705295774068735529074794881754614149"},
    {"Seahorse dive 2", 1e42, "-0.74363021764435289490180174187040211824405527974597657765728802803897859062",
     "0.13179414728676452841409172905996391973831127024373948657952815558406252255"},
    {"Seahorse dive 3", 1e56, "-0.74363021764435289490180174187040211824405457485109184892410846142003846471",
     "0.13179414728676452841409172905996391973831106931655758831013716385362019770"},
    {"Seahorse dive 4", 1e66, "-0.74363021764435289490180174187040211824405457485109184891454625582422596510",
     "0.13179414728676452841409172905996391973831106931655758830325963653618269806"},
};
constexpr int PLACE_COUNT = sizeof(PLACES) / sizeof(PLACES[0]);

// The two saved views of older versions, until their slots are stored again
Settings::Slot legacy_slots[2] = {};

// The view in a slot: stored with its picture, or one of the older ones
bool slot_view(int slot, settings::View& out) {
    if (const settings::View* v = settings::view(slot)) {
        out = *v;
        return true;
    }
    if (slot < 2 && legacy_slots[slot].zoom > 0) {
        out = {};
        out.center = legacy_slots[slot].center;
        out.zoom = legacy_slots[slot].zoom;
        return true;
    }
    return false;
}

// The image at 1/5 of its size, each pixel the average of 5 x 5
void draw_thumbnail(int first_row, int rows, uint16_t* out) {
    constexpr int SCALE = SCREEN_W / settings::THUMBNAIL_W;
    static_assert(SCALE * settings::THUMBNAIL_W == SCREEN_W && SCALE * settings::THUMBNAIL_H == SCREEN_H,
                  "the thumbnail must be the screen scaled down");
    static uint16_t row[SCREEN_W];
    static uint16_t sums[settings::THUMBNAIL_W][3];
    for (int ty = first_row; ty < first_row + rows; ++ty) {
        memset(sums, 0, sizeof(sums));
        for (int dy = 0; dy < SCALE; ++dy) {
            color_palette.render_rows(state.pixelState, state.screen_w, state.screen_h, ty * SCALE + dy, 1, row,
                                      &state.content);
            for (int x = 0; x < SCREEN_W; ++x) {
                uint16_t v = static_cast<uint16_t>((row[x] >> 8) | (row[x] << 8));
                uint16_t* sum = sums[x / SCALE];
                sum[0] += v >> 11;
                sum[1] += (v >> 5) & 63;
                sum[2] += v & 31;
            }
        }
        for (int tx = 0; tx < settings::THUMBNAIL_W; ++tx) {
            constexpr int N = SCALE * SCALE;
            uint16_t v = static_cast<uint16_t>(((sums[tx][0] + N / 2) / N << 11) | ((sums[tx][1] + N / 2) / N << 5)
                                               | (sums[tx][2] + N / 2) / N);
            out[(ty - first_row) * settings::THUMBNAIL_W + tx] = static_cast<uint16_t>((v >> 8) | (v << 8));
        }
    }
}

// A slot whose picture is drawn again once the view is done (it was stored while calculating, or had none)
struct PendingThumbnail {
    int slot = -1;
    uint32_t calculation;
} pending_thumbnail;

bool store_current_view(int slot) {
    settings::View v = {};
    v.center = state.center;
    v.zoom = state.zoom_factor;
    v.has_look = 1;
    v.palette = static_cast<uint8_t>(color_palette.index());
    v.bands = BANDS[bands].id;
    v.orbit_trap = trap_byte();
    v.shading = color_palette.shading;
    v.light = static_cast<uint8_t>(light);
    v.extra_look = extra_look();
    if (!settings::store_view(slot, v, draw_thumbnail)) return false;
    if (slot < 2) legacy_slots[slot].zoom = 0;  // replaced
    pending_thumbnail = {state.calculating ? slot : -1, state.calculation_id};
    return true;
}

// The look a view was stored with
void apply_look(const settings::View& v) {
    apply_extra_look(v.extra_look, v.palette == palette::SOLID);
    color_palette.select(v.palette);
    set_bands_id(v.bands);
    apply_trap_byte(v.orbit_trap);
    color_palette.shading = v.shading;
    set_light(v.light);
}

void go_to_slot(int slot) {
    settings::View v;
    if (!slot_view(slot, v)) {
        toast("Slot %d is empty", slot + 1);
        show_led_feedback(255, 0, 0, 300);
        return;
    }
    if (v.has_look) apply_look(v);
    jump(v.center, v.zoom);
    if (!settings::thumbnail(slot)) pending_thumbnail = {slot, state.calculation_id};
    menu::close();
    show_led_feedback(255, 255, 255, 80);
}

void store_in_slot(int slot) {
    if (store_current_view(slot)) {
        toast("Stored in slot %d", slot + 1);
        show_led_feedback(255, 255, 255, 300);
    } else {
        toast("Storing failed");
        show_led_feedback(255, 0, 0, 300);
    }
}

// ---- USB drive: the views as text (see UsbDrive.hpp and its README.TXT) ----

const char* const USB_KEYWORDS[] = {"current", "slot"};
// Line 0 is the current view, 1 to 10 the slots
constexpr int USB_LINES = 1 + settings::VIEW_SLOTS;
// The lines of VIEWS.TXT, hashed: a line written back unchanged changes nothing
uint32_t usb_line_hash[USB_LINES];

// FNV-1a of the words of a line, the spaces between them don't count
uint32_t line_hash(const char* s) {
    uint32_t h = 2166136261u;
    bool space = false;
    for (; *s; ++s) {
        if (*s == ' ') {
            space = true;
            continue;
        }
        if (space) h = (h ^ ' ') * 16777619u;
        space = false;
        h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    }
    return h;
}

bool same_name(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a == ' ' ? '-' : static_cast<char>(tolower(*a));
        char y = *b == ' ' ? '-' : static_cast<char>(tolower(*b));
        if (x != y) return false;
    }
    return *a == *b;
}

// Names in the text have - instead of spaces
void put_name(char* out, int size, const char* name) {
    int i = 0;
    for (; name[i] && i < size - 1; ++i) out[i] = name[i] == ' ' ? '-' : name[i];
    out[i] = '\0';
}

const char* bands_name(uint8_t id) {
    for (const Bands& b : BANDS) {
        if (b.id == id) return b.name;
    }
    return BANDS[DEFAULT_BANDS].name;
}

// The shortest decimal that reads back as the same value (up to the last bits): typed values stay as they were
// typed, others get all their digits
void format_center(const Fixed& value, char* out, int size) {
    char text[100];
    auto print = [&](int decimals) {
        Fixed half(0.5);
        for (int i = 0; i < decimals; ++i) half = half.divided(10);
        format_coordinate(value.negative() ? value - half : value + half, decimals, 1000, text, sizeof(text));
    };
    auto exact = [&]() {
        Fixed difference = Fixed::parse(text) - value;
        if (difference.negative()) difference = -difference;
        for (int i = 1; i < Fixed::LIMBS; ++i) {
            if (difference.limb[i]) return false;
        }
        return difference.limb[0] <= 4;
    };
    // More decimals are exact as well
    int low = 1, high = 75;
    while (low < high) {
        int middle = (low + high) / 2;
        print(middle);
        if (exact()) high = middle;
        else low = middle + 1;
    }
    print(low);
    const char* start = text[0] == ' ' ? text + 1 : text;
    int n = static_cast<int>(strlen(start));
    while (n > 2 && start[n - 1] == '0' && start[n - 2] != '.') n--;
    snprintf(out, size, "%.*s", n, start);
}

int view_line(char* out, int size, const char* name, const settings::View& v) {
    char re[100], im[100];
    format_center(v.center.real, re, sizeof(re));
    format_center(v.center.imag, im, sizeof(im));
    int n = snprintf(out, size, "%s zoom %.10g re %s im %s", name, v.zoom, re, im);
    if (v.has_look && n < size) {
        char palette_text[24], light_text[24];
        put_name(palette_text, sizeof(palette_text), palette::name(v.palette));
        put_name(light_text, sizeof(light_text), LIGHT_NAMES[v.light % LIGHT_COUNT]);
        n += snprintf(out + n, size - n, " palette %s bands %s trap %s shading %s light %s", palette_text,
                      bands_name(v.bands), TRAP_NAMES[(v.orbit_trap & 15) % Fractalis::TRAP_COUNT],
                      v.shading ? "on" : "off", light_text);
        char distance_text[24];
        put_name(distance_text, sizeof(distance_text), DISTANCE_NAMES[look_distance(v.extra_look)]);
        if (n < size) n += snprintf(out + n, size - n, " distance %s", distance_text);
        if (v.palette == palette::SOLID && n < size) {
            // hue/saturation/brightness, e.g. azure/80/100
            const SolidColor c = look_color(v.extra_look);
            char hue[24];
            n += snprintf(out + n, size - n, " color %s/%d/%d", hue_name(c.hue, hue, sizeof(hue)), 10 * c.saturation,
                          10 * c.brightness);
        }
    }
    return std::min(n, size - 1);
}

settings::View current_view() {
    settings::View v = {};
    v.center = state.center;
    v.zoom = state.zoom_factor;
    v.has_look = 1;
    v.palette = static_cast<uint8_t>(color_palette.index());
    v.bands = BANDS[bands].id;
    v.orbit_trap = trap_byte();
    v.shading = color_palette.shading;
    v.light = static_cast<uint8_t>(light);
    v.extra_look = extra_look();
    return v;
}

// VIEWS.TXT
int usb_views(char* out, int size) {
    int n = snprintf(out, size, "FractalisPico views. Change a line and save, see README.TXT\r\n\r\n");
    char text[300];
    for (int i = 0; i < USB_LINES; ++i) {
        char name[8];
        snprintf(name, sizeof(name), i == 0 ? "current" : "slot%d", i);
        settings::View v;
        if (i == 0) {
            int n = view_line(text, sizeof(text), name, current_view());
            // Where auto zoom dives to, e.g. a minibrot found here
            if (autoZoom.has_target()) snprintf(text + n, sizeof(text) - n, " target %.10g", autoZoom.target_zoom());
        } else if (slot_view(i - 1, v)) {
            view_line(text, sizeof(text), name, v);
        } else {
            snprintf(text, sizeof(text), "%s empty", name);
        }
        usb_line_hash[i] = line_hash(text);
        if (n + static_cast<int>(strlen(text)) + 2 < size) n += snprintf(out + n, size - n, "%s\r\n", text);
    }
    return n;
}

// A line like those of VIEWS.TXT. index: 0 = current, 1 to 10 = slot.
struct UsbLine {
    int index;
    bool empty;
    settings::View view;
    double target;  // zoom auto zoom dives to, 0 = none
};

bool parse_usb_line(const char* text, UsbLine& out) {
    char words[400];
    snprintf(words, sizeof(words), "%s", text);
    char* word[48];
    int count = 0;
    for (char* p = strtok(words, " "); p && count < 48; p = strtok(nullptr, " ")) word[count++] = p;
    if (count == 0) return false;
    out = {};
    if (strcmp(word[0], "current") == 0) {
        out.index = 0;
    } else if (strncmp(word[0], "slot", 4) == 0) {
        char* end;
        long slot = strtol(word[0] + 4, &end, 10);
        if (*end || slot < 1 || slot > settings::VIEW_SLOTS) return false;
        out.index = static_cast<int>(slot);
    } else {
        return false;
    }
    if (count == 2 && strcmp(word[1], "empty") == 0) {
        out.empty = out.index > 0;
        return out.empty;
    }

    // The look of the device for what the line leaves out
    settings::View& v = out.view;
    v = current_view();
    v.has_look = 0;
    bool zoom = false, re = false, im = false;
    auto decimal = [](const char* s, Fixed& value) {
        // -?digits(.digits)?
        const char* p = s + (*s == '-');
        int digits = 0, points = 0;
        for (; *p; ++p) {
            if (*p == '.') points++;
            else if (*p >= '0' && *p <= '9') digits++;
            else return false;
        }
        if (digits == 0 || points > 1 || std::abs(atof(s)) > 16) return false;
        value = Fixed::parse(s);
        return true;
    };
    for (int i = 1; i + 1 < count; i += 2) {
        const char* key = word[i];
        const char* value = word[i + 1];
        bool found = false;
        if (strcmp(key, "zoom") == 0) {
            char* end;
            v.zoom = strtod(value, &end);
            zoom = !*end && std::isfinite(v.zoom) && v.zoom > 0;
            if (!zoom) return false;
        } else if (strcmp(key, "target") == 0) {
            char* end;
            out.target = strtod(value, &end);
            if (*end || !(out.target > 0) || !std::isfinite(out.target)) out.target = 0;
        } else if (strcmp(key, "re") == 0) {
            if (!(re = decimal(value, v.center.real))) return false;
        } else if (strcmp(key, "im") == 0) {
            if (!(im = decimal(value, v.center.imag))) return false;
        } else if (strcmp(key, "palette") == 0) {
            for (int k = 0; k < palette::count() && !found; ++k) {
                if ((found = same_name(value, palette::name(k)))) v.palette = static_cast<uint8_t>(k);
            }
            v.has_look = 1;
        } else if (strcmp(key, "bands") == 0) {
            for (int k = 0; k < BANDS_COUNT && !found; ++k) {
                if ((found = same_name(value, BANDS[k].name))) v.bands = BANDS[k].id;
            }
            v.has_look = 1;
        } else if (strcmp(key, "trap") == 0) {
            for (int k = 0; k < Fractalis::TRAP_COUNT && !found; ++k) {
                if ((found = same_name(value, TRAP_NAMES[k]))) v.orbit_trap = static_cast<uint8_t>(k);
            }
            v.has_look = 1;
        } else if (strcmp(key, "shading") == 0) {
            v.shading = same_name(value, "on");
            v.has_look = 1;
        } else if (strcmp(key, "light") == 0) {
            for (int k = 0; k < LIGHT_COUNT && !found; ++k) {
                if ((found = same_name(value, LIGHT_NAMES[k]))) v.light = static_cast<uint8_t>(k);
            }
            v.has_look = 1;
        } else if (strcmp(key, "distance") == 0) {
            for (int k = 0; k < DISTANCE_COUNT && !found; ++k) {
                if ((found = same_name(value, DISTANCE_NAMES[k]))) {
                    v.extra_look = pack_look(k, look_color(v.extra_look));
                }
            }
            v.has_look = 1;
        } else if (strcmp(key, "color") == 0) {
            // hue/saturation/brightness, the percentages in steps of 10
            char hue[24];
            int saturation, brightness;
            if (sscanf(value, "%23[^/]/%d/%d", hue, &saturation, &brightness) == 3) {
                SolidColor c;
                char buffer[24];
                for (int k = 0; k < HUE_STEPS && !found; ++k) {
                    if ((found = same_name(hue, hue_name(k, buffer, sizeof(buffer))))) c.hue = k;
                }
                c.saturation = std::max(0, std::min((saturation + 5) / 10, 10));
                c.brightness = std::max(0, std::min((brightness + 5) / 10, 10));
                if (found) v.extra_look = pack_look(look_distance(v.extra_look), c);
            }
            v.has_look = 1;
        }
        // Other words are left for later versions
    }
    return zoom && re && im;
}

/**
 * The lines the computer wrote: the slots are stored, the current view is where it goes. Only what differs from
 * VIEWS.TXT, then the drive shows the new state.
 */
void usb_import() {
    char text[400];
    UsbLine current = {};
    bool go = false;
    int stored = 0, last_slot = 0;
    while (usb_drive::next_line(text, sizeof(text))) {
        UsbLine l;
        if (!parse_usb_line(text, l) || line_hash(text) == usb_line_hash[l.index]) continue;
        if (l.index == 0) {
            current = l;
            go = true;
            continue;
        }
        const int slot = l.index - 1;
        if (l.empty ? settings::clear_view(slot) : settings::store_view(slot, l.view, nullptr)) {
            if (slot < 2) legacy_slots[slot].zoom = 0;
            stored++;
            last_slot = slot;
        }
        usb_line_hash[l.index] = line_hash(text);
    }
    if (go) {
        const settings::View& v = current.view;
        if (v.has_look) apply_look(v);
        jump(v.center, v.zoom);
        if (current.target > 0) {
            autoZoom.set_target(state.center, std::min(current.target, PRECISION_MAX_ZOOM));
            target_period = 0;
        }
    }
    if (!go && stored == 0) return;
    if (go && stored)
        long_toast("From USB: the view, %d slot%s", stored, stored > 1 ? "s" : "");
    else if (go)
        long_toast("From USB: went to the view");
    else if (stored == 1)
        long_toast("From USB: slot %d", last_slot + 1);
    else
        long_toast("From USB: %d slots", stored);
    show_led_feedback(255, 255, 255, 300);
    usb_drive::refresh();
}

// ---- Icons ----

const menu::Icon ICON_MANDELBROT = {{
    "................",
    "................",
    "...........+....",
    "...........#+...",
    ".........+####+.",
    "........+######.",
    ".....+#+#######.",
    "++++###########.",
    "++++###########.",
    ".....+#+#######.",
    "........+######.",
    ".........+####+.",
    "...........#+...",
    "...........+....",
    "................",
    "................",
}};
const menu::Icon ICON_PALETTE = {{
    "................",
    ".....######.....",
    "...##########...",
    "..####..######..",
    ".#####..##..###.",
    ".##########..##.",
    "#..############.",
    "#..#############",
    "###########..###",
    ".#######.....##.",
    ".######......##.",
    "..######...###..",
    "...##########...",
    ".....######.....",
    "................",
    "................",
}};
const menu::Icon ICON_DROP = {{
    "................",
    ".......++.......",
    "......+##+......",
    "......####......",
    ".....+####+.....",
    ".....######.....",
    "....+######+....",
    "....########....",
    "...+########+...",
    "...##########...",
    "...##########...",
    "...##########...",
    "...+########+...",
    "....+######+....",
    "......####......",
    "................",
}};
// A dot inside two rings: distance lines around a shape
const menu::Icon ICON_RINGS = {{
    "................",
    ".....+####+.....",
    "...##+....+##...",
    "..#..........#..",
    ".#...+####+...#.",
    ".#..#+....+#..#.",
    "#..#........#..#",
    "#..#...##...#..#",
    "#..#...##...#..#",
    "#..#........#..#",
    ".#..#+....+#..#.",
    ".#...+####+...#.",
    "..#..........#..",
    "...##+....+##...",
    ".....+####+.....",
    "................",
}};
const menu::Icon ICON_SUN = {{
    ".......##.......",
    ".......##.......",
    "..##........##..",
    "..###......###..",
    "......####......",
    ".....######.....",
    "##..########..##",
    "##..########..##",
    ".....######.....",
    "......####......",
    "..###......###..",
    "..##........##..",
    ".......##.......",
    ".......##.......",
    "................",
    "................",
}};
const menu::Icon ICON_SPARKLE = {{
    ".....#..........",
    ".....#..........",
    "....###.........",
    "..#######.......",
    "....###......#..",
    ".....#......###.",
    ".....#.......#..",
    "...........#....",
    "..........###...",
    "........#######.",
    "........#######.",
    "..........###...",
    "...........#....",
    "...#............",
    "..###...........",
    "...#............",
}};
const menu::Icon ICON_ZOOM = {{
    "....#####.......",
    "..##.....##.....",
    ".#.........#....",
    ".#....#....#....",
    "#.....#.....#...",
    "#...#####...#...",
    "#.....#.....#...",
    ".#....#....#....",
    ".#.........#....",
    "..##.....###....",
    "....#####.###...",
    "...........###..",
    "............###.",
    ".............###",
    "..............#.",
    "................",
}};
const menu::Icon ICON_BOOKMARK = {{
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..###########...",
    "..#####.#####...",
    "..####...####...",
    "..###.....###...",
    "..##.......##...",
    "..#.........#...",
    "................",
    "................",
}};
const menu::Icon ICON_GEAR = {{
    "......####......",
    "......####......",
    "..##.######.##..",
    ".##############.",
    "..############..",
    "..####....####..",
    "#####......#####",
    "#####......#####",
    "..####....####..",
    "..############..",
    ".##############.",
    "..##.######.##..",
    "......####......",
    "......####......",
    "................",
    "................",
}};
const menu::Icon ICON_PIN = {{
    ".....######.....",
    "....########....",
    "...###....###...",
    "...##......##...",
    "...##......##...",
    "...###....###...",
    "....########....",
    ".....######.....",
    "......####......",
    "......####......",
    ".......##.......",
    ".......##.......",
    "................",
    "................",
    "................",
    "................",
}};
const menu::Icon ICON_FINDER = {{
    "....#####.......",
    "..##.....##.....",
    ".#.........#....",
    ".#......+..#....",
    "#.....+###..#...",
    "#.++######+.#...",
    "#.....+###..#...",
    ".#......+..#....",
    ".#.........#....",
    "..##.....###....",
    "....#####.###...",
    "...........###..",
    "............###.",
    ".............###",
    "..............#.",
    "................",
}};
const menu::Icon ICON_BARS = {{
    "................",
    "............###.",
    "............###.",
    "........###.###.",
    "........###.###.",
    "........###.###.",
    "....###.###.###.",
    "....###.###.###.",
    "....###.###.###.",
    "###.###.###.###.",
    "###.###.###.###.",
    "###.###.###.###.",
    "................",
    "################",
    "................",
    "................",
}};

// ---- Orbit viewer ----

/**
 * The orbit of the center point, z1, z2, z3 ..., in a small inset of the plane around 0 (the circle |z| = 2 marked).
 * Calculated in fixed point once per view, so it is right at any depth, and drawn into an alpha mask: anti-aliased
 * lines between the points, the points brighter, both fading along the orbit.
 */
constexpr int ORBIT_SIZE = 80;              // pixels, square
constexpr float ORBIT_RANGE = 2.3f;         // |re|, |im| up to this fit in
constexpr int ORBIT_ITERATIONS = 500;       // drawn
// Iterated further, up to the view's iteration limit, to find out if it escapes or where it settles
constexpr int ORBIT_MAX_ITERATIONS = 20000;
constexpr int ORBIT_HISTORY = 128;          // the longest cycle that gets named
// The corner inset: the orbit viewer or the Julia inset, they share the corner and the memory
enum Inset { INSET_OFF, INSET_ORBIT, INSET_JULIA, INSET_COUNT };
const char* const INSET_NAMES[] = {"off", "orbit", "Julia"};
static_assert(sizeof(INSET_NAMES) / sizeof(INSET_NAMES[0]) == INSET_COUNT, "a name for every inset");
int inset = INSET_OFF;
constexpr int JULIA_W = 80, JULIA_H = 60;
uint8_t inset_buffer[JULIA_W * JULIA_H];
static_assert(sizeof(inset_buffer) >= ORBIT_SIZE * ORBIT_SIZE / 2, "room for the orbit");
uint8_t* const orbit_mask = inset_buffer;  // 4 bits per pixel, see menu::mask()
Coordinate orbit_center;
bool orbit_ready = false;
int orbit_cycle = 0;    // length of the cycle the orbit settles into, 0 = none found
int orbit_limit = 0;    // iterations done without escaping
int orbit_escape = 0;   // the iteration it escaped at, 0 = it didn't

float orbit_x(float re) { return (re / ORBIT_RANGE * 0.5f + 0.5f) * ORBIT_SIZE; }
float orbit_y(float im) { return (0.5f - im / ORBIT_RANGE * 0.5f) * ORBIT_SIZE; }

// Adds a point with bilinear weights, the brightest drawing of a pixel wins
void orbit_splat(float x, float y, int alpha) {
    x -= 0.5f;
    y -= 0.5f;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int px = ix + i, py = iy + j;
            if (px < 0 || py < 0 || px >= ORBIT_SIZE || py >= ORBIT_SIZE) continue;
            const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            // A bit more than the weight: a line of splats half a pixel apart is about as bright as alpha
            const int a = std::min(15, static_cast<int>(alpha * w * (1.6f / 17.0f) + 0.5f));
            const int index = py * ORBIT_SIZE + px, shift = 4 * (index & 1);
            uint8_t& m = orbit_mask[index / 2];
            if (a > ((m >> shift) & 15)) m = static_cast<uint8_t>((m & ~(15 << shift)) | (a << shift));
        }
    }
}

void orbit_line(float x0, float y0, float x1, float y1, int alpha) {
    const float length = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::min(400, static_cast<int>(length * 2.0f) + 1);
    for (int k = 0; k <= steps; ++k) {
        const float t = static_cast<float>(k) / steps;
        orbit_splat(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, alpha);
    }
}

void orbit_dot(float x, float y, int alpha) {
    orbit_splat(x, y, alpha);
    for (int k = 0; k < 4; ++k) {
        orbit_splat(x + (k == 0 ? 0.8f : k == 1 ? -0.8f : 0.0f), y + (k == 2 ? 0.8f : k == 3 ? -0.8f : 0.0f),
                    alpha * 2 / 3);
    }
}

void calculate_orbit() {
    orbit_center = state.center;
    orbit_ready = true;
    orbit_cycle = orbit_escape = 0;
    orbit_limit = std::max(ORBIT_ITERATIONS, std::min(static_cast<int>(state.iteration_limit), ORBIT_MAX_ITERATIONS));
    memset(orbit_mask, 0, ORBIT_SIZE * ORBIT_SIZE / 2);
    std::complex<float> history[ORBIT_HISTORY];
    Fixed zr(0.0), zi(0.0);
    float last_x = orbit_x(0.0f), last_y = orbit_y(0.0f);
    for (int n = 1; n <= orbit_limit; ++n) {
        const Fixed zr2 = zr * zr, zi2 = zi * zi;
        zi = (zr * zi).twice() + state.center.imag;
        zr = zr2 - zi2 + state.center.real;
        const float re = static_cast<float>(zr.to_double()), im = static_cast<float>(zi.to_double());
        history[n % ORBIT_HISTORY] = {re, im};
        if (n <= ORBIT_ITERATIONS) {
            // Older points fade, so the start and where it settles can be told apart
            const int alpha = 255 - 170 * n / ORBIT_ITERATIONS;
            const float x = orbit_x(re), y = orbit_y(im);
            orbit_line(last_x, last_y, x, y, alpha / 2);
            orbit_dot(x, y, alpha);
            last_x = x;
            last_y = y;
        }
        if (re * re + im * im > 4.0f) {
            orbit_escape = n;
            return;
        }
    }
    // The shortest k with z_n-k = z_n: the cycle. Near the edge of a bulb the orbit only creeps towards it, so the
    // points count as the same within a thousandth.
    const std::complex<float> last = history[orbit_limit % ORBIT_HISTORY];
    for (int k = 1; k < ORBIT_HISTORY; ++k) {
        if (std::abs(history[(orbit_limit - k) % ORBIT_HISTORY] - last) < 1e-3f) {
            orbit_cycle = k;
            break;
        }
    }
}

bool orbit_shown() {
    return inset == INSET_ORBIT && !menu::is_open();
}

void draw_orbit() {
    if (!orbit_ready) return;
    const int x = SCREEN_W - ORBIT_SIZE - 10, y = SCREEN_H - ORBIT_SIZE - 26;
    const Box box = {x - 4, y - 14, ORBIT_SIZE + 8, ORBIT_SIZE + 18};
    if (!menu::visible(box)) return;
    menu::set_clip({0, 0, SCREEN_W, SCREEN_H});
    menu::round_rect(box, 6, {14, 14, 26}, 205);
    // Axes and |z| = 2
    const float cx = x + orbit_x(0.0f), cy = y + orbit_y(0.0f);
    menu::blend({x, static_cast<int>(cy), ORBIT_SIZE, 1}, {200, 200, 220}, 50);
    menu::blend({static_cast<int>(cx), y, 1, ORBIT_SIZE}, {200, 200, 220}, 50);
    menu::ring(cx, cy, 2.0f / ORBIT_RANGE * 0.5f * ORBIT_SIZE, 1.0f, {200, 200, 220}, 110);
    menu::mask(x, y, ORBIT_SIZE, ORBIT_SIZE, orbit_mask, {255, 225, 120});
    char text[24];
    if (orbit_escape)
        snprintf(text, sizeof(text), "escapes at %d", orbit_escape);
    else if (orbit_cycle)
        snprintf(text, sizeof(text), "cycle of %d", orbit_cycle);
    else
        snprintf(text, sizeof(text), "no escape in %d", orbit_limit);
    menu::text(text, x, box.y + 4, {225, 226, 235}, true);
}

// ---- Julia inset ----

/**
 * The Julia set of the center point, in the corner: z -> z^2 + c with c = the center and z starting at each pixel.
 * Near a minibrot it shows the shape the minibrot's surroundings repeat. Float is enough, deep views barely move c.
 * Calculated by core0 in slices of rows (see work()), one byte per pixel: 0 = in the set, else the color.
 */
constexpr float JULIA_RANGE = 1.6f;  // |re| up to this fits in, the height to scale
constexpr int JULIA_ITERATIONS = 256;
Coordinate julia_center;
bool julia_valid = false;   // the buffer holds the Julia set (or part of it) of julia_center
int julia_row = JULIA_H;    // next row to calculate, JULIA_H = done
int julia_palette = -1;
uint16_t julia_colors[256];  // display byte order

void julia_begin() {
    if (!julia_valid) memset(inset_buffer, 0, sizeof(inset_buffer));
    julia_center = state.center;
    julia_valid = true;
    julia_row = 0;
}

void julia_rows(int rows) {
    const float cr = static_cast<float>(julia_center.real.to_double());
    const float ci = static_cast<float>(julia_center.imag.to_double());
    const float step = 2.0f * JULIA_RANGE / JULIA_W;
    for (const int end = std::min(JULIA_H, julia_row + rows); julia_row < end; ++julia_row) {
        const float im = (julia_row + 0.5f - JULIA_H * 0.5f) * step;  // down, like the image
        for (int x = 0; x < JULIA_W; ++x) {
            float zr = (x + 0.5f - JULIA_W * 0.5f) * step, zi = im;
            float zr2 = zr * zr, zi2 = zi * zi;
            int n = 0;
            while (n < JULIA_ITERATIONS && zr2 + zi2 < 256.0f) {
                zi = 2.0f * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                n++;
            }
            uint8_t value = 0;
            if (n < JULIA_ITERATIONS) {
                // Smooth iteration count, log scale like the main image: a palette cycle every few doublings
                const float smooth = n + 1 - std::log2(std::log2(zr2 + zi2) * 0.5f);
                const float t = std::log2(1.0f + std::max(smooth, 0.0f)) * 0.25f;
                value = static_cast<uint8_t>(1 + static_cast<int>((t - std::floor(t)) * 254.99f));
            }
            inset_buffer[julia_row * JULIA_W + x] = value;
        }
    }
}

bool julia_shown() {
    return inset == INSET_JULIA && !menu::is_open() && julia_valid;
}

// Core0 work: the next rows. Returns false when there's nothing to do.
bool julia_work() {
    if (inset != INSET_JULIA) return false;
    if (!julia_valid || (julia_row >= JULIA_H && memcmp(&julia_center, &state.center, sizeof(Coordinate)) != 0)) {
        julia_begin();
    }
    if (julia_row >= JULIA_H) return false;
    julia_rows(6);
    if (julia_row >= JULIA_H) state.needs_redraw = true;
    return true;
}

void draw_julia() {
    const int x = SCREEN_W - JULIA_W - 10, y = SCREEN_H - JULIA_H - 26;
    const Box box = {x - 4, y - 14, JULIA_W + 8, JULIA_H + 18};
    if (!menu::visible(box)) return;
    if (julia_palette != color_palette.index()) {
        julia_palette = color_palette.index();
        julia_colors[0] = 0;
        for (int i = 1; i < 256; ++i) {
            const uint16_t c = palette::sample(julia_palette, (i - 1) / 255.0f);
            julia_colors[i] = static_cast<uint16_t>((c >> 8) | (c << 8));
        }
    }
    menu::set_clip({0, 0, SCREEN_W, SCREEN_H});
    menu::round_rect(box, 6, {14, 14, 26}, 205);
    uint16_t line[JULIA_W];
    for (int row = 0; row < JULIA_H; ++row) {
        if (!menu::visible({x, y + row, JULIA_W, 1})) continue;
        const uint8_t* values = inset_buffer + row * JULIA_W;
        for (int i = 0; i < JULIA_W; ++i) line[i] = julia_colors[values[i]];
        menu::image(line, x, y + row, JULIA_W, 1);
    }
    menu::text("Julia set", x, box.y + 4, {225, 226, 235}, true);
}

int inset_get(int) { return inset; }
void inset_set(int, int v) {
    inset = v;
    orbit_ready = false;
    julia_valid = false;
}
int inset_count(int) { return INSET_COUNT; }
const char* inset_text(int, int option, char*, int) { return INSET_NAMES[option]; }


// ---- Minibrot glow ----

/**
 * A soft halo over minibrots too small to see. Once a view is done, core0 probes a grid of cells: the ball method
 * finds the biggest minibrot in the disc around each (see MinibrotFinder::probe()). The ones smaller than a few
 * pixels glow, bigger ones are visible anyway. Shares the finder with the minibrot search, which goes first.
 */
constexpr int GLOW_CELL = 20;                       // pixels, square
constexpr int GLOW_COLUMNS = SCREEN_W / GLOW_CELL;  // 16
constexpr int GLOW_ROWS = SCREEN_H / GLOW_CELL;     // 12
constexpr int GLOW_MAX = 64;                        // the biggest ones
constexpr float GLOW_VISIBLE_PX = 32.0f;            // bigger minibrots are easy to see, they don't glow
constexpr Color GLOW_COLOR = {170, 235, 255};
struct GlowSpot {
    float x, y;  // on screen
    float px;    // size of the minibrot in pixels
};
bool glow_on = false;
GlowSpot glow_spots[GLOW_MAX];
int glow_count = 0;
uint32_t glow_calculation = 0;  // the view scanned (or being scanned)
bool glow_done = false;

// Width of a pixel in the complex plane, like search_starts()
double glow_pixel() { return 4.0 / state.zoom_factor / state.screen_w; }

// The complex coordinate of a point on screen (pixel centers at .5)
Coordinate glow_point(float x, float y) {
    Coordinate c = state.center;
    const double pixel = glow_pixel();
    c.real += Fixed((x - state.screen_w * 0.5) * pixel);
    c.imag += Fixed((y - state.screen_h * 0.5) * pixel);
    return c;
}

// Cells that are all in the set (or not calculated) have nothing hidden that the image doesn't show
bool glow_cell_worth(int cell) {
    const int x0 = cell % GLOW_COLUMNS * GLOW_CELL, y0 = cell / GLOW_COLUMNS * GLOW_CELL;
    for (int y = y0; y < y0 + GLOW_CELL && y < state.screen_h; ++y) {
        for (int x = x0; x < x0 + GLOW_CELL && x < state.screen_w; ++x) {
            if (state.pixelState[y][x].hasPosition()) return true;
        }
    }
    return false;
}

void glow_next_cell() {
    while (++glow_cell < GLOW_COLUMNS * GLOW_ROWS) {
        if (!glow_cell_worth(glow_cell)) continue;
        const float cx = (glow_cell % GLOW_COLUMNS + 0.5f) * GLOW_CELL, cy = (glow_cell / GLOW_COLUMNS + 0.5f) * GLOW_CELL;
        finder.probe(glow_point(cx, cy), GLOW_CELL * 0.7071 * glow_pixel());
        return;
    }
    glow_cell = -1;
    glow_done = true;
    state.needs_redraw = true;
}

// A probe is done: keeps the minibrot if it lies in its cell (so none counts twice) and is too small to see
void glow_probe_done() {
    if (!finder.found()) return;
    const MinibrotFinder::Result& r = finder.result();
    const double pixel = glow_pixel();
    const float x = static_cast<float>((r.nucleus.real - state.center.real).to_double() / pixel + state.screen_w * 0.5);
    const float y = static_cast<float>((r.nucleus.imag - state.center.imag).to_double() / pixel + state.screen_h * 0.5);
    const int cx = glow_cell % GLOW_COLUMNS, cy = glow_cell / GLOW_COLUMNS;
    if (!(x >= cx * GLOW_CELL && x < (cx + 1) * GLOW_CELL && y >= cy * GLOW_CELL && y < (cy + 1) * GLOW_CELL)) return;
    // It fills the screen at its zoom, the set is about 2.5 of the 4 units wide
    const float px = static_cast<float>(state.screen_w * state.zoom_factor / r.zoom() * 0.625);
    if (!(px < GLOW_VISIBLE_PX)) return;
    // Sorted, biggest first
    int i = std::min(glow_count, GLOW_MAX - 1);
    if (glow_count == GLOW_MAX && px <= glow_spots[i].px) return;
    for (; i > 0 && glow_spots[i - 1].px < px; --i) glow_spots[i] = glow_spots[i - 1];
    glow_spots[i] = {x, y, px};
    glow_count = std::min(glow_count + 1, GLOW_MAX);
}

void glow_clear() {
    if (glow_cell >= 0) finder.stop();
    glow_cell = -1;
    glow_count = 0;
    glow_done = false;
    glow_calculation = state.calculation_id;
}

// Core0 work: one slice of the scan. Returns false when there's nothing to do.
bool glow_work(bool (*interrupt)()) {
    if (!glow_on) return false;
    if (state.calculation_id != glow_calculation) {
        const bool shown = glow_count > 0;
        glow_clear();
        if (shown) state.needs_redraw = true;
    }
    if (glow_done || state.calculating) return false;
    if (glow_cell < 0) glow_next_cell();
    const uint32_t until = now_ms() + CORE0_WORK_MS;
    while (glow_cell >= 0 && static_cast<int32_t>(until - now_ms()) > 0 && !(interrupt && interrupt())) {
        if (!finder.work(256)) {
            glow_probe_done();
            glow_next_cell();
        }
    }
    return true;
}

bool glow_shown() {
    return glow_on && glow_done && glow_count > 0 && !menu::is_open();
}

void draw_glow() {
    menu::set_clip({0, 0, SCREEN_W, SCREEN_H});
    for (int i = 0; i < glow_count; ++i) {
        const GlowSpot& s = glow_spots[i];
        // Bigger minibrots glow bigger and brighter: 0 for the tiniest, 1 at the size where they become visible
        const float strength = std::clamp(std::log2(s.px * 32768.0f / GLOW_VISIBLE_PX) / 15.0f, 0.0f, 1.0f);
        const float radius = 6.0f + 16.0f * strength;
        if (!menu::visible({static_cast<int>(s.x - radius), static_cast<int>(s.y - radius),
                            static_cast<int>(2 * radius) + 2, static_cast<int>(2 * radius) + 2}))
            continue;
        // Discs on top of each other: a fade towards the edge
        constexpr int LAYERS = 8;
        const int alpha = static_cast<int>(30 + 24 * strength);
        for (int k = LAYERS; k >= 1; --k) menu::disc(s.x, s.y, radius * k / LAYERS, GLOW_COLOR, alpha);
    }
}

int glow_get(int) { return glow_on; }
void glow_set(int, int v) {
    glow_on = v != 0;
    glow_clear();
    state.needs_redraw = true;
}

// ---- Where am I ----

/**
 * What lies under the center point, for the info overlay. Once a view is done, core0
 * 1. iterates the center: escaped (when, and how far from the set by the distance estimate) or the cycle it settles in
 * 2. inside the set: names the bulb it's in (BulbNamer)
 * 3. probes the disc around the screen for the biggest minibrot in view (MinibrotFinder::probe())
 */
constexpr int WHERE_MIN_ITERATIONS = 2000;  // inside, the orbit has to settle in its cycle
constexpr int WHERE_MAX_ITERATIONS = 20000;
BulbNamer namer;
Fixed where_zr, where_zi;   // the orbit at the end
bool where_probe_started = false;
bool where_ready = false;   // the orbit part, for where_calculation
bool where_done = false;    // the probe too
int where_escape = 0;       // iteration it escaped at, 0 = it didn't
float where_distance = 0;   // from the set in pixels, when it escaped
int where_cycle = 0;        // the cycle it settled in, 0 = none found
int where_limit = 0;        // iterations done
bool where_minibrot = false;
int where_period = 0;
double where_minibrot_zoom = 0;
float where_minibrot_px = 0;  // distance of its nucleus from the center, in pixels

void where_orbit() {
    where_escape = where_cycle = 0;
    where_distance = 0;
    where_limit = std::clamp(static_cast<int>(state.iteration_limit), WHERE_MIN_ITERATIONS, WHERE_MAX_ITERATIONS);
    std::complex<float> history[ORBIT_HISTORY];
    Fixed zr(0.0), zi(0.0);
    std::complex<double> dz = 0;
    for (int n = 1; n <= where_limit; ++n) {
        const std::complex<double> z_old(zr.to_double(), zi.to_double());
        dz = 2.0 * z_old * dz + 1.0;
        const Fixed zr2 = zr * zr, zi2 = zi * zi;
        zi = (zr * zi).twice() + state.center.imag;
        zr = zr2 - zi2 + state.center.real;
        std::complex<double> z(zr.to_double(), zi.to_double());
        history[n % ORBIT_HISTORY] = {static_cast<float>(z.real()), static_cast<float>(z.imag())};
        if (std::norm(z) > 4.0) {
            where_escape = n;
            // On in double until |z| is big, the distance estimate needs it: |z| log|z| / |dz|
            const std::complex<double> c(state.center.real.to_double(), state.center.imag.to_double());
            for (int i = 0; i < 64 && std::norm(z) < 1e20; ++i) {
                dz = 2.0 * z * dz + 1.0;
                z = z * z + c;
            }
            const double r = std::abs(z);
            where_distance = static_cast<float>(r * std::log(r) / std::abs(dz) / glow_pixel());
            return;
        }
    }
    where_zr = zr;
    where_zi = zi;
    // Like the orbit viewer
    const std::complex<float> last = history[where_limit % ORBIT_HISTORY];
    for (int k = 1; k < ORBIT_HISTORY && k < where_limit; ++k) {
        if (std::abs(history[(where_limit - k) % ORBIT_HISTORY] - last) < 1e-3f) {
            where_cycle = k;
            break;
        }
    }
}

// Core0 work, returns false when there's nothing to do
bool where_work() {
    if (hud == HUD_OFF) return false;
    if (state.calculation_id != where_calculation) {
        if (where_probing) finder.stop();
        namer.stop();
        where_probing = where_probe_started = false;
        where_ready = where_done = false;
        where_calculation = state.calculation_id;
    }
    if (where_done || state.calculating) return false;
    if (!where_ready) {
        where_orbit();
        where_ready = true;
        if (!where_escape) namer.start(state.center, where_zr, where_zi, where_cycle);
        state.needs_redraw = true;
        return true;
    }
    const uint32_t until = now_ms() + CORE0_WORK_MS;
    if (namer.busy()) {
        while (namer.work(256)) {
            if (static_cast<int32_t>(until - now_ms()) <= 0) return true;
        }
        state.needs_redraw = true;
        return true;
    }
    if (!where_probe_started) {
        where_probe_started = true;
        // The glow scan starts over after the probe
        if (glow_cell >= 0) finder.stop();
        glow_cell = -1;
        glow_done = false;
        where_minibrot = false;
        where_probing = true;
        finder.probe(state.center, 2.0 / state.zoom_factor);
        state.needs_redraw = true;
        return true;
    }
    while (finder.work(256)) {
        if (static_cast<int32_t>(until - now_ms()) <= 0) return true;
    }
    where_probing = false;
    where_done = true;
    if (finder.found()) {
        const MinibrotFinder::Result& r = finder.result();
        where_minibrot = true;
        where_period = r.period;
        where_minibrot_zoom = r.zoom();
        const double dx = (r.nucleus.real - state.center.real).to_double();
        const double dy = (r.nucleus.imag - state.center.imag).to_double();
        where_minibrot_px = static_cast<float>(std::sqrt(dx * dx + dy * dy) / glow_pixel());
    }
    state.needs_redraw = true;
    return true;
}

const char* ordinal_suffix(int n) {
    if (n % 100 >= 11 && n % 100 <= 13) return "th";
    return n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
}

// "main cardioid > 1/3 > 1/2": the chain of bulbs from the cardioid out, ... where it's incomplete
int bulb_path(char* out, int size) {
    int n;
    if (!namer.named())
        n = snprintf(out, size, "...");
    else if (namer.base_period() == 1)
        n = snprintf(out, size, "main cardioid");
    else
        n = snprintf(out, size, "minibrot p%d", namer.base_period());
    for (int i = namer.depth() - 1; i >= 0 && n >= 0 && n < size; --i) {
        n += snprintf(out + n, size - n, " > %d/%d", namer.bulb(i).m, namer.bulb(i).k);
    }
    return n;
}

// The center is in the set: the name of its bulb, once known
int inside_text(char* out, int size) {
    const int period = namer.period() ? namer.period() : where_cycle;
    if (namer.busy() || (!namer.named() && namer.depth() == 0)) {
        if (period) return snprintf(out, size, "Here: inside, cycle of %d", period);
        return snprintf(out, size, "Here: no escape in %d", where_limit);
    }
    const int depth = namer.depth();
    if (depth == 0) {
        if (namer.base_period() == 1) return snprintf(out, size, "Here: in the main cardioid");
        return snprintf(out, size, "Here: in minibrot p%d", namer.base_period());
    }
    if (depth == 1 && namer.named()) {
        const BulbNamer::Bulb& b = namer.bulb(0);
        if (namer.base_period() == 1)
            return snprintf(out, size, "Here: %d/%d bulb of the main cardioid, p%d", b.m, b.k, period);
        return snprintf(out, size, "Here: %d/%d bulb of minibrot p%d, p%d", b.m, b.k, namer.base_period(), period);
    }
    int n = namer.named() ? snprintf(out, size, "Here: %d%s order bulb, period %d", depth, ordinal_suffix(depth), period)
                          : snprintf(out, size, "Here: inside, period %d", period);
    if (n < 0 || n >= size) return n;
    n += snprintf(out + n, size - n, "\nPath: ");
    if (n >= size) return n;
    return n + bulb_path(out + n, size - n);
}

// The lines for the info overlay, empty while it's not known yet
void where_text(char* out, int size) {
    out[0] = 0;
    if (!where_ready || state.calculation_id != where_calculation) return;
    int n;
    if (where_escape && where_distance >= 1.0f)
        n = snprintf(out, size, "Here: outside, %.0f px from the set", where_distance);
    else if (where_escape)
        n = snprintf(out, size, "Here: on the edge, escapes at %d", where_escape);
    else
        n = inside_text(out, size);
    // Period 1 is the main set itself
    if (n < 0 || n >= size || !where_done || !where_minibrot || where_period < 2) return;
    char zoom[16];
    format_deep_zoom(zoom, sizeof(zoom), where_minibrot_zoom);
    if (where_minibrot_px < 1.0f)
        snprintf(out + n, size - n, "\nOn minibrot p%d, fills screen at %s", where_period, zoom);
    else
        snprintf(out + n, size - n, "\nMinibrot p%d %.0f px off, fills at %s", where_period, where_minibrot_px, zoom);
}

// ---- Menu rows ----

constexpr Color LIGHT_TEXT = {225, 226, 235};

// Each column the palette at its position, cycles times through it over the box
void swatch(int palette_index, float cycles, const Box& box) {
    if (!menu::visible(box)) return;
    uint16_t colors[64];
    const int w = std::min(box.w, 64);
    for (int i = 0; i < w; ++i) colors[i] = palette::sample(palette_index, (i + 0.5f) / w * cycles);
    menu::columns({box.x, box.y + 2, w, box.h - 4}, colors);
}

const char* names(const char* const* list, int option) {
    return list[option];
}

// Colors
int palette_get(int) { return color_palette.index(); }
void palette_set(int, int v) { color_palette.select(v); }
int palette_count(int) { return palette::count(); }
const char* palette_text(int, int option, char*, int) { return palette::name(option); }
void palette_decor(int, int option, const Box& box) { swatch(option, 1.0f, box); }

int bands_get(int) { return bands; }
void bands_set(int, int v) { set_bands(v); }
int bands_count(int) { return BANDS_COUNT; }
const char* bands_text(int, int option, char*, int) { return BANDS[option].name; }
void bands_decor(int, int option, const Box& box) { swatch(color_palette.index(), BANDS[option].factor, box); }

int cycle_get(int) { return static_cast<int>(color_cycle); }
void cycle_set(int, int v) { color_cycle = static_cast<ColorCycle>(v); }
int cycle_count(int) { return 3; }
const char* cycle_text(int, int option, char*, int) { return names(COLOR_CYCLE_NAMES, option); }

int cycle_speed_get(int) { return cycle_speed; }
void cycle_speed_set(int, int v) { cycle_speed = v; }
int cycle_speed_count(int) { return CYCLE_SPEED_COUNT; }
const char* cycle_speed_text(int, int option, char*, int) { return CYCLE_SPEEDS[option].name; }

// A color box with a soft frame, so black and dark colors show on the panel as well
void color_swatch(uint16_t color, const Box& box) {
    if (!menu::visible(box)) return;
    const Color c = {static_cast<uint8_t>((color >> 11) * 255 / 31),
                     static_cast<uint8_t>(((color >> 5) & 63) * 255 / 63),
                     static_cast<uint8_t>((color & 31) * 255 / 31)};
    menu::round_rect({box.x, box.y + 1, box.w, box.h - 2}, 3, menu::SOFT_TEXT, 90);
    menu::round_rect({box.x + 1, box.y + 2, box.w - 2, box.h - 4}, 2, c);
}

// Solid color, param: 0 hue, 1 saturation, 2 brightness
int& solid_part(SolidColor& c, int param) {
    return param == 0 ? c.hue : param == 1 ? c.saturation : c.brightness;
}
int solid_get(int param) { return solid_part(solid, param); }
void solid_set(int param, int v) {
    solid_part(solid, param) = v;
    apply_solid_color();
    // Picking a color shows it
    if (color_palette.index() != palette::SOLID) color_palette.select(palette::SOLID);
}
int solid_count(int param) { return param == 0 ? HUE_STEPS : 11; }
const char* solid_text(int param, int option, char* buffer, int size) {
    if (param == 0) return hue_name(option, buffer, size);
    snprintf(buffer, size, "%d %%", 10 * option);
    return buffer;
}
// The hue at full color, the others the color that option gives
void solid_decor(int param, int option, const Box& box) {
    SolidColor c = solid;
    if (param == 0) c = {option, 10, 10};
    else solid_part(c, param) = option;
    color_swatch(solid_rgb(c), box);
}
void solid_page_decor(int, int, const Box& box) { color_swatch(solid_rgb(solid), box); }

int distance_get(int) { return distance; }
void distance_set(int, int v) { set_distance(v); }
int distance_count(int) { return DISTANCE_COUNT; }
const char* distance_text(int, int option, char*, int) { return DISTANCE_NAMES[option]; }
// The width of the lines
void distance_decor(int, int option, const Box& box) {
    if (option < 2) return;
    const int width = option - 1;
    menu::fill({box.x + box.w / 2 - 6, box.y + box.h / 2 - width / 2, 13, width}, LIGHT_TEXT);
}

int trap_get(int) { return fractalis.orbit_trap(); }
int region_get(int) { return fractalis.orbit_trap_region(); }
void region_set(int, int v) {
    fractalis.set_orbit_trap(fractalis.orbit_trap(), v);
    update_outlines();
}
int region_count(int) { return Fractalis::REGION_COUNT; }
const char* region_text(int, int option, char*, int) { return REGION_NAMES[option]; }
void trap_set(int, int v) {
    fractalis.set_orbit_trap(v);
    update_outlines();
}
int trap_count(int) { return Fractalis::TRAP_COUNT; }
const char* trap_text(int, int option, char*, int) { return names(TRAP_NAMES, option); }
void trap_decor(int, int option, const Box& box) {
    const float cx = box.x + box.w / 2.0f, cy = box.y + box.h / 2.0f;
    switch (option) {
        case Fractalis::TRAP_POINT:
            menu::disc(cx, cy, 2.5f, LIGHT_TEXT);
            break;
        case Fractalis::TRAP_CROSS:
            menu::fill({box.x + box.w / 2 - 5, box.y + box.h / 2, 11, 1}, LIGHT_TEXT);
            menu::fill({box.x + box.w / 2, box.y + box.h / 2 - 5, 1, 11}, LIGHT_TEXT);
            break;
        case Fractalis::TRAP_RING:
            menu::ring(cx, cy, 5.0f, 1.3f, LIGHT_TEXT);
            break;
        case Fractalis::TRAP_PERIOD:
            // Three bulbs in a row, getting smaller
            menu::disc(cx - 4.0f, cy, 3.0f, LIGHT_TEXT);
            menu::disc(cx + 1.5f, cy, 1.8f, LIGHT_TEXT);
            menu::disc(cx + 4.5f, cy, 1.0f, LIGHT_TEXT);
            break;
        case Fractalis::TRAP_STRIPE:
            for (int k = -1; k <= 1; ++k) menu::fill({box.x + box.w / 2 - 5, box.y + box.h / 2 + 3 * k, 11, 1}, LIGHT_TEXT);
            break;
    }
}

// Light
int shading_get(int) { return color_palette.shading; }
int edges_get(int) { return color_palette.edges; }
void edges_set(int, int v) { color_palette.edges = v != 0; }
void shading_set(int, int v) { color_palette.shading = v != 0; }

int light_get(int) { return light; }
void light_set(int, int v) { set_light(v); }
int light_count(int) { return LIGHT_COUNT; }
const char* light_text(int, int option, char*, int) { return names(LIGHT_NAMES, option); }
// A dial with the sun where the light comes from
void light_decor(int, int option, const Box& box) {
    const float cx = box.x + box.w / 2.0f, cy = box.y + box.h / 2.0f;
    const float angle = option == LIGHT_ROTATING ? light_angle : LIGHT_ANGLES[option];
    menu::ring(cx, cy, 5.5f, 1.2f, menu::SOFT_TEXT);
    menu::disc(cx + 5.0f * std::cos(angle), cy + 5.0f * std::sin(angle), 2.3f, {255, 205, 70});
}

// Rendering
int ss_get(int) {
    for (int i = 0; i < SUPERSAMPLING_COUNT; ++i) {
        if (SUPERSAMPLING[i] == fractalis.supersampling()) return i;
    }
    return 0;
}
void ss_set(int, int v) { fractalis.set_supersampling(SUPERSAMPLING[v]); }
int ss_count(int) { return SUPERSAMPLING_COUNT; }
const char* ss_text(int, int option, char* buffer, int size) {
    snprintf(buffer, size, option == 0 ? "off" : "%dx", SUPERSAMPLING[option]);
    return buffer;
}

int right_away_get(int) { return fractalis.supersample_right_away(); }
void right_away_set(int, int v) { fractalis.set_supersample_right_away(v != 0); }

int probes_get(int) { return fractalis.show_probes(); }
void probes_set(int, int v) { fractalis.set_show_probes(v != 0); }

int set_display_get(int) { return fractalis.undecided_in_set() ? 0 : 1; }
void set_display_set(int, int v) { fractalis.set_undecided_in_set(v == 0); }
int two(int) { return 2; }
const char* set_display_text(int, int option, char*, int) { return option == 0 ? "shrinking" : "preview"; }

// Auto zoom
const char* auto_zoom_label(int, char*, int) {
    return state.auto_zoom ? "Stop auto zoom" : autoZoom.has_target() ? "Dive to the minibrot" : "Start auto zoom";
}
void auto_zoom_run(int) { set_auto_zoom(!state.auto_zoom); }

int step_get(int) { return autoZoom.speed_index(); }
void step_set(int, int v) { autoZoom.set_speed(v); }
int step_count(int) { return AutoZoom::speed_count(); }
const char* step_text(int, int option, char*, int) { return AutoZoom::speed_name(option); }

int pause_get(int) { return autoZoom.pause_index(); }
void pause_set(int, int v) { autoZoom.set_pause(v); }
int pause_count(int) { return AutoZoom::pause_count(); }
const char* pause_text(int, int option, char*, int) { return AutoZoom::pause_name(option); }

int full_quality_get(int) { return autoZoom.full_quality(); }
void full_quality_set(int, int v) { autoZoom.set_full_quality(v != 0); }

// Views
bool has_history(int) { return history_count > 0; }
const char* history_text(int, int, char* buffer, int size) {
    if (history_count == 0) return "";
    format_zoom(buffer, size, history[history_count - 1].zoom);
    return buffer;
}
void go_back(int) {
    if (history_count == 0) return;
    const PastView v = history[--history_count];
    fractalis.set_view(v.center, v.zoom);
}

void reset_view(int) {
    remember_view();
    fractalis.reset_view();
}

const char* place_label(int i, char*, int) { return PLACES[i].name; }
const char* place_text(int i, int, char* buffer, int size) {
    format_zoom(buffer, size, PLACES[i].zoom);
    return buffer;
}
void place_run(int i) {
    jump({Fixed::parse(PLACES[i].real), Fixed::parse(PLACES[i].imag)}, PLACES[i].zoom);
}

const char* depth_label(int i, char* buffer, int size) {
    snprintf(buffer, size, "x1e%d", 10 * (i + 1));
    return buffer;
}
// Not shallower than the current view
bool depth_enabled(int i) { return finder_depth(i) > state.zoom_factor; }
// A gauge from zoom 1 to the precision limit (log scale): filled down to the depth, a mark at the current zoom
void depth_decor(int i, int, const Box& box) {
    if (!menu::visible(box)) return;
    const double full = std::log10(PRECISION_MAX_ZOOM);
    const int x = box.x + 2, w = box.w - 4, y = box.y + box.h / 2 - 2;
    menu::round_rect({x, y, w, 5}, 2, {62, 64, 84});
    menu::round_rect({x, y, static_cast<int>(w * 10 * (i + 1) / full + 0.5), 5}, 2, menu::color());
    const double here = std::log10(std::max(1.0, state.zoom_factor)) / full;
    menu::fill({x + static_cast<int>(w * std::min(here, 1.0)), y - 3, 2, 11}, {255, 255, 255});
}
void depth_run(int i) { find_minibrot(i); }


void usb_run(int) {
    usb_drive::refresh();
    toast("USB drive: back in a few seconds");
}
const char* usb_text(int, int, char*, int) { return usb_drive::present() ? "" : "away"; }

void slot_run(int slot) { go_to_slot(slot); }
void slot_hold(int slot) { store_in_slot(slot); }

// The picture of the view, its number, zoom and look
void slot_row(int slot, int selected, const Box& box) {
    if (!menu::visible(box)) return;
    constexpr int W = settings::THUMBNAIL_W, H = settings::THUMBNAIL_H;
    settings::View v;
    const bool used = slot_view(slot, v);
    const uint16_t* thumbnail = used ? settings::thumbnail(slot) : nullptr;
    const int x = box.x + 2, y = box.y + (box.h - H) / 2;
    if (thumbnail) {
        menu::image(thumbnail, x, y, W, H);
    } else {
        // An outline, a picture comes once the view is done there
        constexpr Color OUTLINE = {120, 122, 140};
        menu::blend({x, y, W, 1}, OUTLINE, 120);
        menu::blend({x, y + H - 1, W, 1}, OUTLINE, 120);
        menu::blend({x, y + 1, 1, H - 2}, OUTLINE, 120);
        menu::blend({x + W - 1, y + 1, 1, H - 2}, OUTLINE, 120);
        if (used) menu::text("?", x + W / 2 - 2, y + H / 2 - 4, menu::SOFT_TEXT);
    }
    const int tx = x + W + 9;
    char text[32];
    snprintf(text, sizeof(text), "Slot %d", slot + 1);
    menu::text(text, tx, box.y + 9, selected ? Color{255, 255, 255} : menu::TEXT);
    if (used) {
        format_zoom(text, sizeof(text), v.zoom);
        menu::text(text, tx, box.y + 22, menu::SOFT_TEXT);
        if (v.has_look) {
            const int trap = v.orbit_trap & 15;
            snprintf(text, sizeof(text), "%s%s%s%s", palette::name(v.palette), trap ? ", " : "",
                     trap ? TRAP_NAMES[trap % Fractalis::TRAP_COUNT] : "",
                     look_distance(v.extra_look) ? ", distance" : "");
            menu::text(text, tx, box.y + 35, menu::SOFT_TEXT);
        }
    } else {
        menu::text("empty", tx, box.y + 22, menu::SOFT_TEXT);
    }
}

// System
int hud_get(int) { return hud; }
void hud_set(int, int v) { hud = static_cast<uint8_t>(v); }
int hud_count(int) { return HUD_COUNT; }
const char* hud_text(int, int option, char*, int) { return names(HUD_NAMES, option); }

void save_settings_now(int);

// Statistics of the current view, fetched once per frame
Fractalis::ViewStats stats;

const char* stat_time(int, int, char* buffer, int size) {
    snprintf(buffer, size, state.calculating ? "%.1f s ..." : "%.2f s", stats.ms / 1000.0);
    return buffer;
}
const char* stat_iterations(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%.1f M", stats.iterations / 1e6);
    return buffer;
}
const char* stat_cycles(int, int, char* buffer, int size) {
    if (stats.cycles_per_iteration == 0) return "-";
    snprintf(buffer, size, "%lu", static_cast<unsigned long>(stats.cycles_per_iteration));
    return buffer;
}
const char* stat_passes(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%d", stats.passes);
    return buffer;
}
const char* stat_pixels(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%.1f k", stats.pixels / 1e3);
    return buffer;
}
const char* stat_reference(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%d, %lu ms", stats.ref_orbits, static_cast<unsigned long>(stats.ref_ms));
    return buffer;
}
const char* stat_frames(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%lu, %lu ms", static_cast<unsigned long>(view_frames.frames),
             static_cast<unsigned long>(view_frames.drawing_us / 1000));
    return buffer;
}
const char* stat_limit(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%d", static_cast<int>(state.iteration_limit));
    return buffer;
}
const char* precision_name() {
    return state.zoom_factor < FLOAT_MAX_ZOOM ? "float"
         : state.zoom_factor < PERTURBATION_MIN_ZOOM ? "double"
         : state.zoom_factor < SCALED_PERTURBATION_MIN_ZOOM ? "perturbation"
         : state.zoom_factor < PRECISION_MAX_ZOOM ? "deep perturbation"
         : "past precision limit!";
}
const char* stat_precision(int, int, char*, int) { return precision_name(); }
const char* stat_clock(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%lu MHz", static_cast<unsigned long>(system_clock_khz() / 1000));
    return buffer;
}
const char* stat_ram(int, int, char* buffer, int size) {
    snprintf(buffer, size, "%u KB", free_ram() / 1024);
    return buffer;
}
const char* stat_uptime(int, int, char* buffer, int size) {
    uint32_t s = now_ms() / 1000;
    snprintf(buffer, size, "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
             static_cast<unsigned long>(s / 60 % 60), static_cast<unsigned long>(s % 60));
    return buffer;
}

// ---- Pages ----

constexpr Color COLORS_COLOR = {200, 70, 255};
constexpr Color STYLE_COLOR = {50, 215, 190};
constexpr Color LIGHT_COLOR = {255, 185, 40};
constexpr Color RENDERING_COLOR = {80, 220, 120};
constexpr Color AUTO_ZOOM_COLOR = {40, 195, 255};
constexpr Color VIEWS_COLOR = {255, 85, 140};
constexpr Color SYSTEM_COLOR = {150, 162, 200};

constexpr const char* STATS_HELP = "Of the current view, while it is calculated";
constexpr menu::Item STATS_ITEMS[] = {
    menu::info("View time", stat_time, STATS_HELP),
    menu::info("Iterations", stat_iterations, STATS_HELP),
    menu::info("Cycles/iteration", stat_cycles, STATS_HELP),
    menu::info("Passes", stat_passes, STATS_HELP),
    menu::info("Pixels calculated", stat_pixels, STATS_HELP),
    menu::info("Reference orbits", stat_reference, STATS_HELP),
    menu::info("Frames drawn", stat_frames, STATS_HELP),
    menu::info("Iteration limit", stat_limit, STATS_HELP),
    menu::info("Precision", stat_precision, STATS_HELP),
    menu::info("Clock", stat_clock, "The speed of the processor"),
    menu::info("Free RAM", stat_ram, "Memory left"),
    menu::info("Uptime", stat_uptime, "Time since the start"),
};
constexpr menu::Page STATS_PAGE = {"Statistics", &ICON_BARS, SYSTEM_COLOR, STATS_ITEMS,
                                   sizeof(STATS_ITEMS) / sizeof(STATS_ITEMS[0])};

constexpr menu::Item place(int i) {
    return menu::action(nullptr, place_run, "A: go there").with_label(place_label).with_value(place_text)
        .with_param(i).closing();
}
constexpr menu::Item PLACE_ITEMS[] = {place(0), place(1), place(2), place(3), place(4), place(5),
                                      place(6), place(7), place(8), place(9), place(10)};
static_assert(sizeof(PLACE_ITEMS) / sizeof(PLACE_ITEMS[0]) == PLACE_COUNT, "a row for every place");
constexpr menu::Page PLACES_PAGE = {"Places", &ICON_PIN, VIEWS_COLOR, PLACE_ITEMS, PLACE_COUNT};

constexpr menu::Item depth(int i) {
    return menu::action(nullptr, depth_run, "Centers a minibrot that fills the screen at this zoom")
        .with_label(depth_label).with_param(i).when(depth_enabled).decorated(depth_decor, 60).closing();
}
constexpr menu::Item FINDER_ITEMS[] = {
    menu::toggle("Glow", glow_get, glow_set, "A soft glow over minibrots too small to see"),
    depth(0), depth(1), depth(2), depth(3), depth(4), depth(5), depth(6)};
static_assert(sizeof(FINDER_ITEMS) / sizeof(FINDER_ITEMS[0]) == 1 + FINDER_DEPTHS, "a row for every depth");
constexpr menu::Page FINDER_PAGE = {"Minibrots", &ICON_FINDER, VIEWS_COLOR, FINDER_ITEMS, 1 + FINDER_DEPTHS};

constexpr menu::Item SOLID_ITEMS[] = {
    menu::choice("Hue", solid_get, solid_set, solid_count, solid_text,
                 "Around the color wheel, shows with saturation").live().decorated(solid_decor, 24),
    menu::choice("Saturation", solid_get, solid_set, solid_count, solid_text, "From gray or white to full color")
        .live().decorated(solid_decor, 24).with_param(1),
    menu::choice("Brightness", solid_get, solid_set, solid_count, solid_text, "From black to bright")
        .live().decorated(solid_decor, 24).with_param(2),
};
constexpr menu::Page SOLID_PAGE = {"Solid color", &ICON_DROP, COLORS_COLOR, SOLID_ITEMS,
                                   sizeof(SOLID_ITEMS) / sizeof(SOLID_ITEMS[0])};

constexpr menu::Item COLORS_ITEMS[] = {
    menu::choice("Palette", palette_get, palette_set, palette_count, palette_text, "The colors of the image")
        .live().decorated(palette_decor, 36),
    menu::page("Solid color", SOLID_PAGE, "One color instead of a palette, picking one selects it")
        .decorated(solid_page_decor, 24),
    menu::choice("Bands", bands_get, bands_set, bands_count, bands_text,
                 "How often the palette repeats over the image").live().decorated(bands_decor, 36),
    menu::choice("Color cycle", cycle_get, cycle_set, cycle_count, cycle_text,
                 "When the colors flow through the image").live(),
    menu::choice("Cycle speed", cycle_speed_get, cycle_speed_set, cycle_speed_count, cycle_speed_text,
                 "Time for one round through the palette").live(),
};
constexpr menu::Page COLORS_PAGE = {"Colors", &ICON_PALETTE, COLORS_COLOR, COLORS_ITEMS,
                                    sizeof(COLORS_ITEMS) / sizeof(COLORS_ITEMS[0])};

// What the colors come from: the iteration count, the distance or an orbit trap
constexpr menu::Item STYLE_ITEMS[] = {
    menu::choice("Distance", distance_get, distance_set, distance_count, distance_text,
                 "Outside colored by the distance to the set, with outlines. Slower").decorated(distance_decor, 16),
    menu::choice("Orbit trap", trap_get, trap_set, trap_count, trap_text,
                 "Colors by how close the orbit comes to a shape").decorated(trap_decor, 12),
    menu::choice("Trap colors", region_get, region_set, region_count, region_text,
                 "Where the orbit trap colors, the rest is colored as usual"),
};
constexpr menu::Page STYLE_PAGE = {"Style", &ICON_RINGS, STYLE_COLOR, STYLE_ITEMS,
                                   sizeof(STYLE_ITEMS) / sizeof(STYLE_ITEMS[0])};

constexpr menu::Item LIGHT_ITEMS[] = {
    menu::toggle("Shading", shading_get, shading_set, "Relief: the image lit like a landscape"),
    menu::toggle("Edge glow", edges_get, edges_set, "Bright outlines where the colors change fast"),
    menu::choice("Direction", light_get, light_set, light_count, light_text, "Where the light comes from")
        .live().decorated(light_decor, 12),
};
constexpr menu::Page LIGHT_PAGE = {"Light", &ICON_SUN, LIGHT_COLOR, LIGHT_ITEMS,
                                   sizeof(LIGHT_ITEMS) / sizeof(LIGHT_ITEMS[0])};

constexpr menu::Item RENDERING_ITEMS[] = {
    menu::choice("Supersampling", ss_get, ss_set, ss_count, ss_text,
                 "More samples per pixel when done: smoother"),
    menu::toggle("Right away", right_away_get, right_away_set,
                 "All samples in every pass: no change at the end"),
    menu::toggle("Show probes", probes_get, probes_set, "Draws the probe points as soon as they are done"),
    menu::choice("Set display", set_display_get, set_display_set, two, set_display_text,
                 "Undecided pixels: black (shrinking) or in color"),
};
constexpr menu::Page RENDERING_PAGE = {"Rendering", &ICON_SPARKLE, RENDERING_COLOR, RENDERING_ITEMS,
                                       sizeof(RENDERING_ITEMS) / sizeof(RENDERING_ITEMS[0])};

constexpr menu::Item AUTO_ZOOM_ITEMS[] = {
    menu::action(nullptr, auto_zoom_run, "Dives into the most detailed area, step by step")
        .with_label(auto_zoom_label).closing(),
    menu::choice("Step", step_get, step_set, step_count, step_text, "Zoom per step. Fly: continuous, less detail")
        .live(),
    menu::choice("Pause", pause_get, pause_set, pause_count, pause_text, "Time to look at a finished view").live(),
    menu::toggle("Full quality", full_quality_get, full_quality_set, "Waits for supersampling before the next step"),
};
constexpr menu::Page AUTO_ZOOM_PAGE = {"Auto zoom", &ICON_ZOOM, AUTO_ZOOM_COLOR, AUTO_ZOOM_ITEMS,
                                       sizeof(AUTO_ZOOM_ITEMS) / sizeof(AUTO_ZOOM_ITEMS[0])};

constexpr int SLOT_ROW_H = settings::THUMBNAIL_H + 6;
constexpr const char* SLOT_HELP = "A: go there\nHold A: store the view here";
constexpr menu::Item slot(int i) {
    return menu::action(nullptr, slot_run, SLOT_HELP).with_hold(slot_hold).with_param(i).with_height(SLOT_ROW_H)
        .drawn_by(slot_row);
}
constexpr menu::Item VIEWS_ITEMS[] = {
    menu::action("Previous view", go_back, "Back to where you were before the last jump").with_value(history_text)
        .when(has_history).closing(),
    menu::action("Reset view", reset_view, "Back to the whole set").closing(),
    menu::page("Places", PLACES_PAGE, "Famous spots and deep zoom tests"),
    menu::page("Minibrots", FINDER_PAGE, "Finds one at the zoom you choose, auto zoom dives there"),
    menu::action("Update USB drive", usb_run, "Puts the views and the picture on the USB drive, it reconnects")
        .with_value(usb_text),
    slot(0), slot(1), slot(2), slot(3), slot(4), slot(5), slot(6), slot(7), slot(8), slot(9),
};
static_assert(sizeof(VIEWS_ITEMS) / sizeof(VIEWS_ITEMS[0]) == 5 + settings::VIEW_SLOTS, "a row for every slot");
constexpr menu::Page VIEWS_PAGE = {"Views", &ICON_BOOKMARK, VIEWS_COLOR, VIEWS_ITEMS,
                                   sizeof(VIEWS_ITEMS) / sizeof(VIEWS_ITEMS[0])};

constexpr menu::Item SYSTEM_ITEMS[] = {
    menu::choice("Info overlay", hud_get, hud_set, hud_count, hud_text,
                 "Coordinates and zoom. Auto: 5 s after a press").live(),
    menu::choice("Corner inset", inset_get, inset_set, inset_count, inset_text,
                 "The orbit of the center point, or the Julia set of it, in a corner"),
    menu::action("Save settings now", save_settings_now, "Otherwise saved 5 min after the last press"),
    menu::page("Statistics", STATS_PAGE, "Time and work of the current view"),
};
constexpr menu::Page SYSTEM_PAGE = {"System", &ICON_GEAR, SYSTEM_COLOR, SYSTEM_ITEMS,
                                    sizeof(SYSTEM_ITEMS) / sizeof(SYSTEM_ITEMS[0])};

constexpr menu::Item MAIN_ITEMS[] = {
    menu::page("Colors", COLORS_PAGE, "Palette or solid color, bands, color cycling"),
    menu::page("Style", STYLE_PAGE, "Distance outlines and orbit traps"),
    menu::page("Light", LIGHT_PAGE, "Relief shading and where the light comes from"),
    menu::page("Rendering", RENDERING_PAGE, "Supersampling and how the image builds up"),
    menu::page("Auto zoom", AUTO_ZOOM_PAGE, "Dives on its own into the most detailed area"),
    menu::page("Views", VIEWS_PAGE, "Saved views, places, minibrots and the way back"),
    menu::page("System", SYSTEM_PAGE, "Info overlay, saving and statistics"),
};
constexpr menu::Page MAIN_PAGE = {"Fractalis", &ICON_MANDELBROT, {225, 226, 240}, MAIN_ITEMS,
                                  sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0])};

// ---- Exploring: pan, zoom, quick functions ----

void set_auto_zoom(bool on) {
    if (on && !state.auto_zoom) {
        remember_view();
        autoZoom.start();
    }
    state.auto_zoom = on;
    printf("Auto Zoom: %d\n", on);
}

// B, X, Y: tap pans left/right or zooms in, long press pans down/up or zooms out
void navigate(Input input, int button) {
    const bool tap = input == Input::PRESS;
    switch (button) {
        case 1:
            fractalis.pan(tap ? -PAN_CONSTANT : 0, tap ? 0 : PAN_CONSTANT);
            break;
        case 2:
            fractalis.pan(tap ? PAN_CONSTANT : 0, tap ? 0 : -PAN_CONSTANT);
            break;
        case 3:
            // Zooming out exactly undoes a zoom in
            fractalis.zoom(tap ? ZOOM_CONSTANT : 1.0 / (1.0 + ZOOM_CONSTANT) - 1.0);
            break;
    }
    if (tap)
        show_led_feedback(0, 0, 255, 50);
    else
        show_led_feedback(200, 0, 255, 100);
}

// A + B: reset view, A + X: next palette, A + Y: auto zoom on/off
void quick(int button) {
    switch (button) {
        case 1:
            reset_view(0);
            toast("Reset view");
            break;
        case 2:
            color_palette.next();
            toast("Palette: %s", color_palette.name());
            break;
        case 3:
            set_auto_zoom(!state.auto_zoom);
            toast("%s", !state.auto_zoom ? "Auto zoom off"
                        : autoZoom.has_target() ? "Diving to the minibrot" : "Auto zoom on");
            break;
    }
    show_led_feedback(255, 255, 255, 80);
}

// ---- Info overlay ----

// The texts of the current frame. Collected once per frame, then drawn into every strip they touch.
struct OverlayText {
    char text[220];
    Point position;
    const bitmap::font_t* font;
    int height;  // all lines and the shadow
};
constexpr int MAX_OVERLAY_TEXTS = 12;
OverlayText overlay_texts[MAX_OVERLAY_TEXTS];
int overlay_text_count = 0;

// What the last frame showed
struct Shown {
    bool overlay, hud, quick, toast;
    uint32_t seconds_to_step;
} shown;

bool hud_visible(uint32_t now) {
    if (menu::is_open()) return false;
    return hud == HUD_ON || (hud == HUD_AUTO && now - last_input_ms < INFO_AUTO_HIDE_MS);
}

void add_text(PicoGraphics& g, const char* text, Point position) {
    if (overlay_text_count >= MAX_OVERLAY_TEXTS || *text == '\0') return;
    OverlayText& t = overlay_texts[overlay_text_count++];
    snprintf(t.text, sizeof(t.text), "%s", text);
    t.position = position;
    t.font = g.bitmap_font;
    int lines = 1;
    for (const char* c = text; *c; ++c) lines += *c == '\n';
    t.height = lines * t.font->height + 1;
}

void render_overlay(PicoGraphics& g, uint32_t now) {
    const int margin = 5;
    g.set_font(&font6);
    const int font_height = font6.height;

    // What the buttons do: the quick functions while A is held
    const char* text_a = "Menu / hold: Quick";
    const char* text_b = "Left / hold: Down";
    const char* text_x = "Right / hold: Up";
    const char* text_y = "Zoom / hold: Out";
    if (quick_shown) {
        text_a = "[Quick]";
        text_b = "> Reset view";
        text_x = "Next palette <";
        text_y = state.auto_zoom ? "Stop auto zoom <" : "Start auto zoom <";
    }
    add_text(g, text_a, Point(margin, margin));
    add_text(g, text_b, Point(margin, SCREEN_H - font_height - margin));
    add_text(g, text_x, Point(SCREEN_W - g.measure_text(text_x, 1) - margin, margin));
    add_text(g, text_y, Point(SCREEN_W - g.measure_text(text_y, 1) - margin, SCREEN_H - font_height - margin));
    if (!hud_visible(now)) return;

    // Coordinates and zoom factor
    g.set_font(&font8);
    const int font8_height = font8.height;
    // Enough decimals to tell neighboring pixels apart
    double pixel_size = 4.0 / state.zoom_factor / state.screen_w;
    int decimals = std::max(6, std::min(74, static_cast<int>(std::ceil(-std::log10(pixel_size))) + 1));
    int line_length = (SCREEN_W - 2 * margin) / g.measure_text("0", 1);
    char real_text[100];
    char imag_text[100];
    format_coordinate(state.center.real, decimals, line_length, real_text, sizeof(real_text));
    format_coordinate(state.center.imag, decimals, line_length, imag_text, sizeof(imag_text));

    char coord_text[sizeof(OverlayText::text)];
    snprintf(coord_text, sizeof(coord_text), "Coordinates:\n%s\n%s", real_text, imag_text);
    int coord_lines = 1;
    for (const char* c = coord_text; *c; ++c) coord_lines += *c == '\n';

    char zoom_text[30];
    if (state.zoom_factor < 1e3)
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.2f", state.zoom_factor);
    else
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.1e", state.zoom_factor);

    int info_y = margin * 3 + font8_height;
    add_text(g, coord_text, Point(margin, info_y));
    info_y += font8_height * coord_lines + margin;
    add_text(g, zoom_text, Point(margin, info_y));

    info_y += font8_height + margin;
    char iterations_text[48];
    snprintf(iterations_text, sizeof(iterations_text), "Iterations: %d (%s)", state.iteration_limit,
             precision_name());
    add_text(g, iterations_text, Point(margin, info_y));

    if (autoZoom.has_target()) {
        info_y += font8_height + margin;
        char zoom_text[16], target_text[48];
        format_deep_zoom(zoom_text, sizeof(zoom_text), autoZoom.target_zoom());
        if (target_period > 0)
            snprintf(target_text, sizeof(target_text), "Minibrot at %s, period %d", zoom_text, target_period);
        else
            snprintf(target_text, sizeof(target_text), "Auto zoom target: %s", zoom_text);
        add_text(g, target_text, Point(margin, info_y));
    }

    char where[sizeof(OverlayText::text)];
    where_text(where, sizeof(where));
    if (where[0]) {
        info_y += font8_height + margin;
        add_text(g, where, Point(margin, info_y));
        for (const char* c = where; *c; ++c) info_y += *c == '\n' ? font8_height : 0;
    }

    if (state.auto_zoom) {
        info_y += font8_height + margin;
        char auto_zoom_text[40];
        uint32_t seconds = autoZoom.seconds_to_next_step(now);
        if (seconds > 1) {
            snprintf(auto_zoom_text, sizeof(auto_zoom_text), "Auto Zoom: ON, next step in %lu:%02lu",
                     static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
        } else {
            snprintf(auto_zoom_text, sizeof(auto_zoom_text), "Auto Zoom: ON");
        }
        add_text(g, auto_zoom_text, Point(margin, info_y));
    }
}

void draw_overlay_texts(PicoGraphics& g, int first_row, int rows) {
    g.clip = Rect(0, first_row, SCREEN_W, rows);
    for (int i = 0; i < overlay_text_count; ++i) {
        const OverlayText& t = overlay_texts[i];
        if (t.position.y >= first_row + rows || t.position.y + t.height <= first_row) continue;
        // White text with a dark shadow, so it is readable on bright colors as well
        g.set_font(t.font);
        // Only the lines in this strip: drawing a text goes through all of its pixels, also the clipped ones
        const int line_height = t.font->height;
        int line_y = t.position.y;
        for (const char* line = t.text; *line;) {
            const char* end = strchr(line, '\n');
            size_t length = end ? static_cast<size_t>(end - line) : strlen(line);
            if (line_y < first_row + rows && line_y + line_height + 1 > first_row) {
                char buffer[sizeof(OverlayText::text)];
                memcpy(buffer, line, length);
                buffer[length] = '\0';
                g.set_pen(0, 0, 0);
                g.text(buffer, Point(t.position.x + 1, line_y + 1), SCREEN_W, 1);
                g.set_pen(255, 255, 255);
                g.text(buffer, Point(t.position.x, line_y), SCREEN_W, 1);
            }
            line_y += line_height;
            if (!end) break;
            line = end + 1;
        }
    }
}

// A short message at the bottom of the image
void draw_toast() {
    const int left = menu::is_open() ? menu::PANEL_W : 0;
    const int w = menu::text_width(toast_text) + 20;
    const Box box = {(left + SCREEN_W - w) / 2, SCREEN_H - 44, w, 21};
    if (!menu::visible(box)) return;
    menu::round_rect(box, 7, {14, 14, 26}, 215);
    menu::text(toast_text, box.x + 10, box.y + 7, {240, 241, 246});
}

void save_settings_now(int) {
    Settings current = current_settings();
    if (settings::save(current)) {
        saved = current;
        toast("Settings saved");
        show_led_feedback(255, 255, 255, 300);
    } else {
        toast("Saving failed");
        show_led_feedback(255, 0, 0, 300);
    }
}

}  // namespace

void start(bool defaults) {
    bool loaded = settings::load(saved);
    fractalis.reset_view();
    if (defaults) {
        printf("B held: starting with the default settings\n");
        memcpy(legacy_slots, saved.slots, sizeof(legacy_slots));  // the saved views stay
    } else if (loaded) {
        printf("Restoring the saved settings\n");
        apply_settings(saved);
    }
    if (!loaded) saved = current_settings();
    menu::set_root(MAIN_PAGE);
    usb_drive::start(usb_image_row, usb_views, USB_KEYWORDS, sizeof(USB_KEYWORDS) / sizeof(USB_KEYWORDS[0]),
                     usb_serial());
}

void handle(Input input, int button) {
    last_input_ms = now_ms();
    state.needs_redraw = true;
    const bool was_open = menu::is_open();
    // Moving the view stops a minibrot search (the menu doesn't)
    if (user_search() && !was_open
            && (input == Input::PRESS || input == Input::LONG || input == Input::REPEAT || input == Input::QUICK)) {
        finder.stop();
        toast("Search stopped");
        return;
    }
    switch (input) {
        case Input::PRESS:
        case Input::LONG:
        case Input::REPEAT:
            if (!was_open) navigate(input, button);
            break;
        case Input::QUICK:
            if (!was_open) quick(button);
            break;
        case Input::OPEN:
            menu::open();
            break;
        case Input::UP:
            menu::up();
            break;
        case Input::DOWN:
            menu::down();
            break;
        case Input::OK:
            menu::ok();
            break;
        case Input::HOLD:
            menu::hold();
            break;
        case Input::BACK:
            menu::back();
            break;
        case Input::CLOSE:
            menu::close();
            break;
    }
    // Only on the way from open to closed: an OPEN of the interrupt may still be waiting in the queue
    if (was_open && !menu::is_open()) menu_active = false;
}

bool update(uint32_t now, uint32_t elapsed_ms) {
    usb_drive::update(now);
    usb_import();

    if (menu::is_open() && now - last_input_ms >= MENU_TIMEOUT_MS) {
        menu::close();
        menu_active = false;
        state.needs_redraw = true;
    }

    // Redraw when the overlay changes by itself
    if (shown.overlay != overlay_wanted() || shown.hud != hud_visible(now) || shown.quick != quick_shown
            || shown.toast != toast_active(now)
            || (shown.hud && state.auto_zoom && shown.seconds_to_step != autoZoom.seconds_to_next_step(now))) {
        state.needs_redraw = true;
    }

    // The search shows that it's still busy, every second
    if (user_search() && (now - search_started_ms) / 1000 != search_seconds) {
        search_seconds = (now - search_started_ms) / 1000;
        long_toast("Looking for a minibrot... %lu s", static_cast<unsigned long>(search_seconds));
    }

    // Draws the picture of a slot again once its view is done
    if (pending_thumbnail.slot >= 0) {
        if (state.calculation_id != pending_thumbnail.calculation) {
            pending_thumbnail.slot = -1;
        } else if (!state.calculating) {
            int slot = pending_thumbnail.slot;
            pending_thumbnail.slot = -1;
            store_current_view(slot);
        }
    }

    // Save what changed. During auto zoom the view changes all the time, it is saved once in a while.
    static uint32_t last_save_ms = 0;
    if (now - last_input_ms >= SAVE_DELAY_MS
            && (!state.auto_zoom || now - last_save_ms >= AUTO_ZOOM_SAVE_INTERVAL_MS)) {
        Settings current = current_settings();
        if (memcmp(&current, &saved, sizeof(Settings)) != 0) {
            settings::save(current);
            saved = current;
            last_save_ms = now;
        }
    }

    bool animating = false;
    bool cycling = color_cycle == ColorCycle::ALWAYS || (color_cycle == ColorCycle::AUTO_ZOOM && state.auto_zoom);
    if (cycling) {
        // Backwards through the palette: the colors move inwards, towards the set
        color_phase -= elapsed_ms / (1000.0f * CYCLE_SPEEDS[cycle_speed].seconds);
        color_phase -= std::floor(color_phase);
        color_palette.set_phase(color_phase);
        animating = true;
    }
    if (light == LIGHT_ROTATING && color_palette.shading) {
        light_angle += elapsed_ms * LIGHT_ROTATION_SPEED / 1000.0f;
        light_angle -= 2.0f * PI * std::floor(light_angle / (2.0f * PI));
        color_palette.set_light(light_angle);
        animating = true;
    }
    return animating;
}

bool work(bool (*interrupt)()) {
    if (!user_search()) return julia_work() || where_work() || glow_work(interrupt);
    const uint32_t until = now_ms() + CORE0_WORK_MS;
    while (finder.busy() && static_cast<int32_t>(until - now_ms()) > 0 && !(interrupt && interrupt())) {
        finder.work(256);
    }
    if (!finder.busy()) search_done();
    return true;
}

bool searching() {
    return user_search();
}

bool overlay_wanted() {
    const uint32_t now = now_ms();
    return menu::is_open() || quick_shown || hud_visible(now) || toast_active(now) || orbit_shown() || julia_shown()
        || glow_shown();
}

void led_color(uint8_t& r, uint8_t& g, uint8_t& b) {
    Color c = {0, 255, 0};
    if (menu::is_open())
        c = menu::color();
    else if (quick_shown)
        c = {200, 0, 255};
    else if (user_search())
        c = {255, 60, 160};
    else if (state.calculating)
        c = {255, 150, 0};
    else if (state.auto_zoom)
        c = {0, 255, 150};
    r = c.r;
    g = c.g;
    b = c.b;
}

void prepare_frame(PicoGraphics& g) {
    const uint32_t now = now_ms();
    overlay_text_count = 0;
    shown = {overlay_wanted(), hud_visible(now), quick_shown, toast_active(now),
             autoZoom.seconds_to_next_step(now)};
    if (orbit_shown() && (!orbit_ready || memcmp(&orbit_center, &state.center, sizeof(Coordinate)) != 0)) {
        calculate_orbit();
    }
    if (menu::is_open()) {
        stats = fractalis.view_stats();
        menu::prepare(g);
    } else if (shown.hud || shown.quick) {
        render_overlay(g, now);
    }
}

void draw_strip(PicoGraphics& g, uint16_t* strip, int first_row, int rows) {
    if (!shown.overlay) return;
    draw_overlay_texts(g, first_row, rows);
    menu::begin_strip(g, strip, first_row, rows);
    if (glow_shown()) draw_glow();
    menu::draw();
    if (orbit_shown()) draw_orbit();
    if (julia_shown()) draw_julia();
    if (shown.toast) {
        menu::set_clip({0, 0, SCREEN_W, SCREEN_H});
        draw_toast();
    }
}

Settings current_settings() {
    Settings s{};
    s.center = state.center;
    s.zoom = state.zoom_factor;
    s.palette = static_cast<uint8_t>(color_palette.index());
    s.shading = color_palette.shading;
    s.supersampling = static_cast<uint8_t>(fractalis.supersampling());
    s.color_cycle = static_cast<uint8_t>(color_cycle);
    s.bands = BANDS[bands].id;
    s.orbit_trap = trap_byte();
    s.light = static_cast<uint8_t>(light);
    s.hud = hud;
    s.auto_zoom = state.auto_zoom;
    s.auto_zoom_speed = static_cast<uint8_t>(autoZoom.speed_index());
    s.auto_zoom_pause = static_cast<uint8_t>(autoZoom.pause_index());
    s.auto_zoom_full_quality = state.auto_zoom_full_quality;
    s.show_probes = fractalis.show_probes();
    s.supersample_right_away = fractalis.supersample_right_away();
    s.set_display_preview = !fractalis.undecided_in_set();
    s.color_cycle_speed = static_cast<uint8_t>(cycle_speed == DEFAULT_CYCLE_SPEED ? 0 : cycle_speed + 1);
    memcpy(s.slots, legacy_slots, sizeof(s.slots));
    s.extra_look = extra_look();
    s.edge_glow = color_palette.edges;
    return s;
}

// Invalid values (e.g. from a broken record) fall back to the defaults
void apply_settings(const Settings& s) {
    bool view_valid = std::isfinite(s.zoom) && s.zoom > 0;
    if (view_valid) fractalis.set_view(s.center, s.zoom);
    apply_extra_look(s.extra_look, true);
    color_palette.select(s.palette);
    color_palette.edges = s.edge_glow;
    color_palette.shading = s.shading;
    fractalis.set_supersampling(s.supersampling);
    color_cycle = s.color_cycle < 3 ? static_cast<ColorCycle>(s.color_cycle) : ColorCycle::ALWAYS;
    cycle_speed = s.color_cycle_speed >= 1 && s.color_cycle_speed <= CYCLE_SPEED_COUNT ? s.color_cycle_speed - 1
                                                                                      : DEFAULT_CYCLE_SPEED;
    set_bands_id(s.bands);
    apply_trap_byte(s.orbit_trap);
    set_light(s.light < LIGHT_COUNT ? s.light : 0);
    if (light == LIGHT_ROTATING) color_palette.set_light(light_angle);
    hud = s.hud < HUD_COUNT ? s.hud : HUD_ON;
    autoZoom.set_speed(s.auto_zoom_speed);
    autoZoom.set_pause(s.auto_zoom_pause);
    state.auto_zoom_full_quality = s.auto_zoom_full_quality;
    fractalis.set_show_probes(s.show_probes);
    if (s.supersample_right_away) fractalis.set_supersample_right_away(true);
    fractalis.set_undecided_in_set(!s.set_display_preview);
    memcpy(legacy_slots, s.slots, sizeof(legacy_slots));
    // A running auto zoom continues where it was
    state.auto_zoom = s.auto_zoom && view_valid;
    if (state.auto_zoom) autoZoom.start();
}

void format_coordinate(const Fixed& value, int decimals, int line_length, char* out, size_t length) {
    size_t n = 0;
    int column = 0;
    auto put = [&](char c) {
        if (column == line_length && n + 4 < length) {
            out[n++] = '\n';
            out[n++] = ' ';
            out[n++] = ' ';
            column = 2;
        }
        if (n + 1 < length) out[n++] = c;
        column++;
    };

    put(value.negative() ? '-' : ' ');
    Fixed magnitude = value.negative() ? -value : value;
    // The integer bits are the top bits of the most significant limb
    constexpr int INTEGER_SHIFT = Fixed::FRACTION_BITS - 32 * (Fixed::LIMBS - 1);
    constexpr uint32_t FRACTION_MASK = (1u << INTEGER_SHIFT) - 1;
    uint32_t& top = magnitude.limb[Fixed::LIMBS - 1];
    char integer_text[8];
    snprintf(integer_text, sizeof(integer_text), "%lu", static_cast<unsigned long>(top >> INTEGER_SHIFT));
    for (const char* c = integer_text; *c; ++c) put(*c);
    put('.');
    for (int i = 0; i < decimals; ++i) {
        top &= FRACTION_MASK;
        magnitude = magnitude.times(10);
        put(static_cast<char>('0' + (top >> INTEGER_SHIFT)));
    }
    out[n] = '\0';
}

}  // namespace ui
