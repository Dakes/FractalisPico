#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/stdio.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "libraries/bitmap_fonts/bitmap_fonts.hpp"
#include "rgbled.hpp"
#include "button.hpp"
#include "FractalisState.h"
#include "fractalis.h"
#include "AutoZoom.hpp"
#include "globals.h"
#include "palette.h"
#include "doubledouble.h"
#include "StripDisplay.hpp"
#include "Settings.hpp"
#include <cmath>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

using namespace pimoroni;
using namespace doubledouble;

// Set to the width and height of your pimoroni display
const uint16_t width = 320;
const uint16_t height = 240;

ST7789 st7789(width, height, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
// RGB565 for smooth color gradients (RGB332 only has 256 colors), drawn in strips instead of a full frame buffer
StripDisplay display(st7789.width, st7789.height, get_spi_pins(BG_SPI_FRONT).spi);
RGBLED led(PicoDisplay::LED_R, PicoDisplay::LED_G, PicoDisplay::LED_B);
Button button_a(PicoDisplay::A);
Button button_b(PicoDisplay::B);
Button button_x(PicoDisplay::X);
Button button_y(PicoDisplay::Y);

FractalisState state(width, height);
Fractalis fractalis(&state);
AutoZoom autoZoom(&state, &fractalis);
palette::Palette color_palette;

repeating_timer_t button_timer;
extern volatile uint8_t event_head;
extern volatile uint8_t event_tail;
// Button feedback on the LED is shown until this time
uint32_t led_hold_until_ms = 0;
// Settings are saved a moment after the last button press
uint32_t last_input_ms = 0;
// Info overlay, toggled with A
bool hud_enabled = true;
// While A is held, B, X and Y have other functions (layer 1). Tap and then hold A for the visual effects (layer 2),
// tap twice and hold for more visual effects (layer 3), three times for the auto zoom settings (layer 4).
volatile uint8_t function_layer = 0;  // written by the button interrupt
uint8_t function_mode = 0;            // what is currently displayed

enum class ColorCycle : uint8_t { AUTO_ZOOM, ALWAYS, OFF };
ColorCycle color_cycle = ColorCycle::ALWAYS;
const char* const COLOR_CYCLE_NAMES[] = {"auto zoom", "always", "off"};
const char* const TRAP_NAMES[] = {"off", "point", "cross", "ring"};
// Visual 2: palette repetitions and light direction (the last one rotates)
// Starts at 1x, cycles up to 4x and then from the fewest bands back to 1x
constexpr float BANDS[] = {1.0f, 2.0f, 4.0f, 0.25f, 0.5f};
const char* const BANDS_NAMES[] = {"1x", "2x", "4x", "0.25x", "0.5x"};
constexpr int BANDS_COUNT = sizeof(BANDS) / sizeof(BANDS[0]);
int bands = 0;
constexpr float PI = 3.14159265f;
constexpr float LIGHT_ANGLES[] = {-0.75f * PI, -0.25f * PI, 0.25f * PI, 0.75f * PI};
const char* const LIGHT_NAMES[] = {"top left", "top right", "bottom right", "bottom left", "rotating"};
constexpr int LIGHT_COUNT = sizeof(LIGHT_NAMES) / sizeof(LIGHT_NAMES[0]);
constexpr int LIGHT_ROTATING = LIGHT_COUNT - 1;
constexpr float LIGHT_ROTATION_SPEED = 2.0f * PI / 12.0f;  // radians per second: a turn in 12 s
int light = 0;
float light_angle = LIGHT_ANGLES[0];
bool overlay_visible = false;

void core1_entry();
void update_display();
void draw_strip(uint16_t* strip, int first_row, int rows);
void update_led();
void render_overlay();
void handle_input();
void help_calculating();
void initialize_rand();
bool sample_buttons(repeating_timer_t*);
Settings current_settings();
void apply_settings(const Settings& s);

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

extern char __StackLimit;  // end of the heap, set by the linker

// Never touched heap plus freed blocks inside the used heap
unsigned free_ram() {
    char* heap_end = static_cast<char*>(sbrk(0));
    return static_cast<unsigned>(&__StackLimit - heap_end) + mallinfo().fordblks;
}

bool overlay_wanted() {
    return hud_enabled || function_mode != 0;
}

void set_clock() {
    if (SYS_CLOCK_KHZ == 0 || !set_sys_clock_khz(SYS_CLOCK_KHZ, false))
        return;
    // The peripheral clock follows the system clock, the display SPI needs its baud rate again.
    // The ST7789 requires 16 ns between SPI rising edges = 62.5 MHz
    spi_set_baudrate(get_spi_pins(BG_SPI_FRONT).spi, 62'500'000);
}

int main() {
    set_clock();
    // Clear the display RAM before anything else, it is filled with noise after power up.
    // Without a draw callback yet, the display draws black.
    st7789.update(&display);
    st7789.set_backlight(255);
    display.set_drawer(draw_strip);

    if (DEBUG) {
        stdio_init_all();
        // Give a serial terminal a moment to connect, so the first messages aren't lost
        uint32_t start = now_ms();
        while (!stdio_usb_connected() && now_ms() - start < USB_WAIT_MS) {
            sleep_ms(10);
        }
    }
    printf("Starting FractalisPico at %lu kHz\n", static_cast<unsigned long>(clock_get_hz(clk_sys) / 1000));

    led.set_brightness(20);
    printf("Display initialized\n");

    // Holding B while powering on starts with the default settings, they are saved once B is released
    bool defaults = button_b.raw();
    Settings saved;
    bool loaded = settings::load(saved);
    fractalis.reset_view();
    if (defaults) {
        printf("B held: starting with the default settings\n");
        led.set_rgb(255, 255, 255);
        while (button_b.raw()) sleep_ms(10);  // released before the buttons are sampled, so it doesn't pan
    } else if (loaded) {
        printf("Restoring the saved settings\n");
        apply_settings(saved);
    }
    if (!loaded)
        saved = current_settings();
    printf("Fractal state initialized, free RAM: %u KB\n", free_ram() / 1024);

    add_repeating_timer_ms(-BUTTON_SAMPLE_MS, sample_buttons, nullptr, &button_timer);

    multicore_launch_core1(core1_entry);
    printf("Core1 launched\n");

    printf("Entering main loop on core0\n");
    uint32_t last_frame_ms = 0;
    uint32_t seen_passes = 0;
    bool animating = false;
    float color_phase = 0.0f;
    uint32_t last_loop_ms = now_ms();
    uint32_t seen_calculation = 0;
    uint32_t calculation_started_ms = 0;
    bool was_calculating = false;
    bool was_auto_zoom = false;
    uint32_t shown_seconds_to_step = 0;
    while(true) {
        update_led();
        handle_input();

        uint32_t now = now_ms();
        bool calculating = state.calculating != 0;
        if (state.calculation_id != seen_calculation) {
            seen_calculation = state.calculation_id;
            calculation_started_ms = now;
        }
        if (was_calculating && !calculating) {
            printf("View done in %lu ms\n", static_cast<unsigned long>(now - calculation_started_ms));
        }
        was_calculating = calculating;

        // Auto contrast after every finished pass
        if (state.passes_completed != seen_passes) {
            seen_passes = state.passes_completed;
            color_palette.analyze(state.pixelState, state.screen_w, state.screen_h);
        }
        bool cycling = color_cycle == ColorCycle::ALWAYS || (color_cycle == ColorCycle::AUTO_ZOOM && state.auto_zoom);
        if (cycling && COLOR_CYCLE_SPEED > 0) {
            // Backwards through the palette: the colors move inwards, towards the set
            color_phase -= (now - last_loop_ms) * COLOR_CYCLE_SPEED / 1000.0f;
            color_phase -= std::floor(color_phase);
            color_palette.set_phase(color_phase);
            animating = true;
        }
        if (light == LIGHT_ROTATING && color_palette.shading) {
            light_angle += (now - last_loop_ms) * LIGHT_ROTATION_SPEED / 1000.0f;
            light_angle -= 2.0f * PI * std::floor(light_angle / (2.0f * PI));
            color_palette.set_light(light_angle);
            animating = true;
        }
        last_loop_ms = now;
        if (overlay_visible != overlay_wanted()) {
            state.needs_redraw = true;
        }

        // Redraw right away after input, regularly while calculating or animating
        uint32_t since_frame = now - last_frame_ms;
        if (state.needs_redraw
                || (animating && since_frame >= ANIMATION_INTERVAL_MS)
                || (calculating && since_frame >= FRAME_INTERVAL_MS)) {
            last_frame_ms = now;
            animating = color_palette.animate();
            update_display();
        }

        // Without full quality, auto zoom doesn't wait for supersampling. The view is supersampled once it stops.
        if (state.auto_zoom && !state.needs_redraw)
            autoZoom.dive(now_ms());
        // The countdown of the auto zoom pause in the info overlay
        uint32_t seconds_to_step = autoZoom.seconds_to_next_step(now);
        if (hud_enabled && seconds_to_step != shown_seconds_to_step) {
            shown_seconds_to_step = seconds_to_step;
            state.needs_redraw = true;
        }
        if (was_auto_zoom && !state.auto_zoom)
            fractalis.supersample();
        was_auto_zoom = state.auto_zoom;

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

        if (calculating) {
            help_calculating();
        } else {
            sleep_ms(animating ? 1 : UPDATE_SLEEP);
        }
    }
}

void core1_entry() {
    printf("Core1 started\n");
    // Core0 can then park this core while it writes the settings to the flash
    flash_safe_execute_core_init();
    while(true) {
        if (!fractalis.work()) {
            sleep_ms(1);
        }
    }
}

bool input_pending() {
    return event_head != event_tail;
}

// Core0 calculates pixels as well in between handling input and refreshing the display.
// A button press interrupts it right away, even in the middle of a slow pixel.
void help_calculating() {
    uint32_t until = now_ms() + CORE0_WORK_MS;
    while (now_ms() < until) {
        if (!fractalis.work(input_pending)) {
            sleep_ms(1);
            return;
        }
    }
}

// The overlay texts of the current frame. Collected once per frame, then drawn into every strip they touch.
struct OverlayText {
    char text[120];
    Point position;
    const bitmap::font_t* font;
    int scale;
    int height;  // incl. all lines and the shadow
};
constexpr int MAX_OVERLAY_TEXTS = 12;
OverlayText overlay_texts[MAX_OVERLAY_TEXTS];
int overlay_text_count = 0;

void update_display() {
    state.needs_redraw = false;
    overlay_visible = overlay_wanted();
    overlay_text_count = 0;
    if (overlay_visible)
        render_overlay();
    st7789.update(&display);  // calls draw_strip() for every strip
}

void draw_strip(uint16_t* strip, int first_row, int rows) {
    color_palette.render_rows(state.pixelState, state.screen_w, state.screen_h, first_row, rows, strip);
    for (int i = 0; i < overlay_text_count; ++i) {
        const OverlayText& t = overlay_texts[i];
        if (t.position.y >= first_row + rows || t.position.y + t.height <= first_row)
            continue;
        // White text with a dark shadow, so it is readable on bright colors as well
        display.set_font(t.font);
        display.set_pen(0, 0, 0);
        display.text(t.text, Point(t.position.x + 1, t.position.y + 1), display.bounds.w, t.scale);
        display.set_pen(255, 255, 255);
        display.text(t.text, t.position, display.bounds.w, t.scale);
    }
}

// Adds a text to the overlay of this frame, in the current font
void draw_text(const char* text, Point position, int scale) {
    if (overlay_text_count >= MAX_OVERLAY_TEXTS || *text == '\0')
        return;
    OverlayText& t = overlay_texts[overlay_text_count++];
    snprintf(t.text, sizeof(t.text), "%s", text);
    t.position = position;
    t.font = display.bitmap_font;
    t.scale = scale;
    int lines = 1;
    for (const char* c = text; *c; ++c) lines += *c == '\n';
    t.height = lines * t.font->height * scale + 1;
}

/**
 * Prints a DoubleDouble with the given number of decimals. printf only knows double, which is not enough for
 * deep zooms.
 */
void format_coordinate(DoubleDouble value, int decimals, char* out, size_t length) {
    size_t n = 0;
    auto put = [&](char c) {
        if (n + 1 < length) out[n++] = c;
    };

    if (value.upper < 0) {
        put('-');
        value = -value;
    } else {
        put(' ');
    }
    double integer_part = std::floor(value.upper);
    DoubleDouble fraction = value - integer_part;
    if (fraction.upper < 0) {
        integer_part -= 1;
        fraction = fraction + 1.0;
    }
    char integer_text[24];
    snprintf(integer_text, sizeof(integer_text), "%.0f", integer_part);
    for (const char* c = integer_text; *c; ++c) put(*c);
    put('.');

    for (int i = 0; i < decimals; ++i) {
        fraction = fraction * 10.0;
        int digit = static_cast<int>(std::floor(fraction.upper));
        fraction = fraction - static_cast<double>(digit);
        if (fraction.upper < 0) {
            digit--;
            fraction = fraction + 1.0;
        }
        put(static_cast<char>('0' + std::max(0, std::min(digit, 9))));
    }
    out[n] = '\0';
}

void render_overlay() {
    display.set_font(&font6);
    int scale = 1;
    int font_height = font6.height * scale;
    int margin = 5;

    // Button functionalities
    // Layer 2: visual effects, layer 3: visual effects 2, layer 4: auto zoom settings
    char text_b2[32], text_x2[32], text_y2[32];
    snprintf(text_b2, sizeof(text_b2), "> Shading: %s", color_palette.shading ? "on" : "off");
    snprintf(text_x2, sizeof(text_x2), "Supersampling: %dx <", fractalis.supersampling());
    snprintf(text_y2, sizeof(text_y2), "Color cycle: %s <", COLOR_CYCLE_NAMES[static_cast<int>(color_cycle)]);
    char text_b3[32], text_x3[32], text_y3[32];
    snprintf(text_b3, sizeof(text_b3), "> Bands: %s", BANDS_NAMES[bands]);
    snprintf(text_x3, sizeof(text_x3), "Orbit trap: %s <", TRAP_NAMES[fractalis.orbit_trap()]);
    snprintf(text_y3, sizeof(text_y3), "Light: %s <", LIGHT_NAMES[light]);
    char text_b4[32], text_x4[32], text_y4[32];
    snprintf(text_b4, sizeof(text_b4), "> Pause: %s", autoZoom.pause_name());
    snprintf(text_x4, sizeof(text_x4), "Step: %s <", autoZoom.speed_name());
    snprintf(text_y4, sizeof(text_y4), "Full quality: %s <", autoZoom.full_quality() ? "on" : "off");
    const char* text_a = "UI / hold: Fn / taps+hold: More";
    const char* text_b = "Left / hold: Down";
    const char* text_x = "Right / hold: Up";
    const char* text_y = "Zoom / hold: Out";
    if (function_mode == 1) {
        text_a = "[Functions]";
        text_b = "> Reset view";
        text_x = "Palette <";
        text_y = state.auto_zoom ? "Auto zoom: stop <" : "Auto zoom: start <";
    } else if (function_mode == 2) {
        text_a = "[Visual]";
        text_b = text_b2;
        text_x = text_x2;
        text_y = text_y2;
    } else if (function_mode == 3) {
        text_a = "[Visual 2]";
        text_b = text_b3;
        text_x = text_x3;
        text_y = text_y3;
    } else if (function_mode == 4) {
        text_a = "[Auto zoom]";
        text_b = text_b4;
        text_x = text_x4;
        text_y = text_y4;
    }
    int32_t text_x_width = display.measure_text(text_x, scale, 1);
    int32_t text_y_width = display.measure_text(text_y, scale, 1);
    draw_text(text_a, Point(margin, margin), scale);
    draw_text(text_b, Point(margin, display.bounds.h - font_height - margin), scale);
    draw_text(text_x, Point(display.bounds.w - text_x_width - margin, margin), scale);
    draw_text(text_y, Point(display.bounds.w - text_y_width - margin, display.bounds.h - font_height - margin), scale);

    if (!hud_enabled)
        return;

    // Display coordinates and zoom factor
    display.set_font(&font8);
    int font8_height = font8.height * scale;

    // Enough decimals to tell neighboring pixels apart
    double pixel_size = 4.0 / state.zoom_factor / state.screen_w;
    int decimals = std::max(6, std::min(32, static_cast<int>(std::ceil(-std::log10(pixel_size))) + 1));
    char real_text[48];
    char imag_text[48];
    format_coordinate(state.center.real, decimals, real_text, sizeof(real_text));
    format_coordinate(state.center.imag, decimals, imag_text, sizeof(imag_text));

    char coord_text[120];
    snprintf(coord_text, sizeof(coord_text), "Coordinates:\n%s\n%s", real_text, imag_text);

    char zoom_text[30];
    if (state.zoom_factor < 1e3)
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.2f", state.zoom_factor);
    else
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.1e", state.zoom_factor);

    int info_y = margin*3 + font8_height;
    draw_text(coord_text, Point(margin, info_y), scale);

    info_y += font8_height*3 + margin;
    draw_text(zoom_text, Point(margin, info_y), scale);

    info_y += font8_height + margin;
    const char* precision = state.zoom_factor < FLOAT_MAX_ZOOM ? "float"
                          : state.zoom_factor < PERTURBATION_MIN_ZOOM ? "double"
                          : state.zoom_factor < DOUBLE_DOUBLE_MAX_ZOOM ? "perturbation"
                          : "past precision limit!";
    char iterations_text[48];
    snprintf(iterations_text, sizeof(iterations_text), "Iterations: %d (%s)", state.iteration_limit, precision);
    draw_text(iterations_text, Point(margin, info_y), scale);

    info_y += font8_height + margin;
    char palette_text[40];
    snprintf(palette_text, sizeof(palette_text), "Palette: %s", color_palette.name());
    draw_text(palette_text, Point(margin, info_y), scale);

    if (fractalis.supersampling() > 1) {
        info_y += font8_height + margin;
        char ss_text[40];
        snprintf(ss_text, sizeof(ss_text), "Supersampling: %dx%s", fractalis.supersampling(),
                 state.supersampling ? " (working)" : "");
        draw_text(ss_text, Point(margin, info_y), scale);
    }

    if (state.auto_zoom) {
        info_y += font8_height + margin;
        char auto_zoom_text[40];
        uint32_t seconds = autoZoom.seconds_to_next_step(now_ms());
        if (seconds > 1) {
            snprintf(auto_zoom_text, sizeof(auto_zoom_text), "Auto Zoom: ON, next step in %lu:%02lu",
                     static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
        } else {
            snprintf(auto_zoom_text, sizeof(auto_zoom_text), "Auto Zoom: ON");
        }
        draw_text(auto_zoom_text, Point(margin, info_y), scale);
    }
}

void update_led() {
    if (static_cast<int32_t>(now_ms() - led_hold_until_ms) < 0) {
        return;
    }

    if (function_mode == 1) {
        led.set_rgb(200, 0, 255);
    } else if (function_mode == 2) {
        led.set_rgb(0, 120, 255);
    } else if (function_mode == 3) {
        led.set_rgb(0, 220, 255);
    } else if (function_mode == 4) {
        led.set_rgb(255, 0, 120);
    } else if (state.calculating) {
        led.set_rgb(255, 150, 0);
    } else {
        if (state.auto_zoom)
            led.set_rgb(0, 255, 150);
        else
            led.set_rgb(0, 255, 0);
    }
}

void show_led_feedback(uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms) {
    led.set_rgb(r, g, b);
    led_hold_until_ms = now_ms() + duration_ms;
}

// Short press
// Short press in function mode
void function_pressed(int i) {
    switch (i) {
        case 1: // Button B: back to the start
            printf("Resetting view\n");
            fractalis.reset_view();
            break;
        case 2: // Button X: next palette, no recalculation needed
            color_palette.next();
            printf("Palette: %s\n", color_palette.name());
            break;
        case 3: // Button Y: auto zoom, continues from the current view
            state.auto_zoom = !state.auto_zoom;
            if (state.auto_zoom) autoZoom.start();
            printf("Auto Zoom: %d\n", state.auto_zoom);
            break;
    }
    state.needs_redraw = true;
    show_led_feedback(255, 255, 255, 80);
}

// Short press in function layer 2 (visual effects)
void function2_pressed(int i) {
    switch (i) {
        case 1: // Button B: relief shading
            color_palette.shading = !color_palette.shading;
            break;
        case 2: { // Button X: supersampling 1x - 8x
            static constexpr int STEPS[] = {1, 2, 3, 4, 6, 8};
            constexpr int COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
            int current = 0;
            while (current < COUNT - 1 && STEPS[current] != fractalis.supersampling()) current++;
            fractalis.set_supersampling(STEPS[(current + 1) % COUNT]);
            printf("Supersampling: %dx\n", fractalis.supersampling());
            break;
        }
        case 3: // Button Y: color cycling
            color_cycle = static_cast<ColorCycle>((static_cast<int>(color_cycle) + 1) % 3);
            break;
    }
    state.needs_redraw = true;
    show_led_feedback(255, 255, 255, 80);
}

// Short press in function layer 3 (visual effects 2)
void function3_pressed(int i) {
    switch (i) {
        case 1: // Button B: color bands, no recalculation needed
            bands = (bands + 1) % BANDS_COUNT;
            color_palette.set_bands(BANDS[bands]);
            break;
        case 2: // Button X: orbit trap, recalculates the view
            fractalis.set_orbit_trap((fractalis.orbit_trap() + 1) % Fractalis::TRAP_COUNT);
            printf("Orbit trap: %s\n", TRAP_NAMES[fractalis.orbit_trap()]);
            break;
        case 3: // Button Y: light direction
            light = (light + 1) % LIGHT_COUNT;
            if (light != LIGHT_ROTATING) {
                light_angle = LIGHT_ANGLES[light];
                color_palette.set_light(light_angle);
            }
            break;
    }
    state.needs_redraw = true;
    show_led_feedback(255, 255, 255, 80);
}

// Short press in function layer 4 (auto zoom settings)
void function4_pressed(int i) {
    switch (i) {
        case 1: // Button B: pause after a finished view
            autoZoom.next_pause();
            printf("Auto zoom pause: %s\n", autoZoom.pause_name());
            break;
        case 2: // Button X: zoom step
            autoZoom.next_speed();
            printf("Auto zoom step: %s\n", autoZoom.speed_name());
            break;
        case 3: // Button Y: wait for supersampling as well
            autoZoom.toggle_full_quality();
            printf("Auto zoom full quality: %d\n", autoZoom.full_quality());
            break;
    }
    state.needs_redraw = true;
    show_led_feedback(255, 255, 255, 80);
}

// Short press
void button_pressed(int i) {
    switch (i) {
        case 0: // Button A: show/hide the info overlay
            hud_enabled = !hud_enabled;
            state.needs_redraw = true;
            break;
        case 1: // Button B: Pan Left
            fractalis.pan(-PAN_CONSTANT, 0);
            break;
        case 2: // Button X: Pan Right
            fractalis.pan(PAN_CONSTANT, 0);
            break;
        case 3: // Button Y: Zoom
            fractalis.zoom(ZOOM_CONSTANT);
            break;
    }
    show_led_feedback(0, 0, 255, 50);
}

// Long press of B, X and Y. Repeats while the button is held.
void button_long_pressed(int i) {
    switch (i) {
        case 1: // Button B: Pan Down
            fractalis.pan(0, PAN_CONSTANT);
            break;
        case 2: // Button X: Pan Up
            fractalis.pan(0, -PAN_CONSTANT);
            break;
        case 3: // Button Y: Zoom out, exactly undoes a zoom in
            fractalis.zoom(1.0 / (1.0 + ZOOM_CONSTANT) - 1.0);
            break;
    }
    show_led_feedback(200, 0, 255, 100);
}

// Buttons are sampled by a timer interrupt, so short presses aren't missed while the main loop is busy.
// The interrupt only records events, the actions run in the main loop.
enum class ButtonEvent : uint8_t { PRESS, LONG, REPEAT, FUNCTION, FUNCTION2, FUNCTION3, FUNCTION4 };
struct ButtonEventEntry {
    uint8_t button;
    ButtonEvent event;
};
constexpr int EVENT_QUEUE_SIZE = 32;
volatile ButtonEventEntry event_queue[EVENT_QUEUE_SIZE];
volatile uint8_t event_head = 0;  // written by the interrupt
volatile uint8_t event_tail = 0;  // written by the main loop
void push_event(int button, ButtonEvent event) {
    uint8_t next = (event_head + 1) % EVENT_QUEUE_SIZE;
    if (next == event_tail) return;  // full, drop it
    event_queue[event_head].button = static_cast<uint8_t>(button);
    event_queue[event_head].event = event;
    event_head = next;
}

/**
 * A: short press shows/hides the info overlay. While A is held, B, X and Y have their second function
 *    (B reset view, X next palette, Y auto zoom on/off), like a shift key.
 *    Tap and then hold A for layer 2 (visual): B relief shading, X supersampling (1x - 8x), Y color cycling.
 *    Tap twice and then hold A for layer 3 (visual 2): B color bands, X orbit trap, Y light direction.
 *    Tap three times and then hold A for layer 4 (auto zoom): B pause after a finished view, X zoom step,
 *    Y full quality (wait for supersampling as well).
 * B, X, Y: short press pans left/right or zooms in, long press pans down/up or zooms out and repeats while held.
 */
bool sample_buttons(repeating_timer_t*) {
    struct ButtonTracker {
        bool down;
        bool long_fired;
        bool shifted;   // pressed while A was held
        uint8_t debounce;
        uint32_t pressed_at;
        uint32_t last_repeat;
    };
    static ButtonTracker trackers[4] = {};
    static Button* const buttons[4] = {&button_a, &button_b, &button_x, &button_y};
    static bool a_used = false;  // A was used as shift key, its release doesn't toggle the overlay
    static uint8_t a_layer = 1;  // layer selected by the current A press
    // A short tap toggles the overlay only once it's clear that no further press (for layer 2 or 3) follows.
    // taps: number of quick taps before the current press
    static bool tap_pending = false;
    static uint8_t taps = 0;
    static uint32_t tap_released_at = 0;
    uint32_t now = now_ms();

    for (int i = 0; i < 4; ++i) {
        ButtonTracker& b = trackers[i];
        bool raw = buttons[i]->raw();
        // Debounce: a change only counts once it is stable for a few samples
        if (raw != b.down) {
            if (++b.debounce < BUTTON_DEBOUNCE_SAMPLES) continue;
        }
        b.debounce = 0;

        if (raw && !b.down) {
            bool shifted = i != 0 && trackers[0].down;
            b = {true, false, shifted, 0, now, now};
            if (i == 0) {
                a_used = false;
                a_layer = tap_pending && now - tap_released_at < DOUBLE_TAP_MS ? taps + 1 : 1;
                tap_pending = false;
            } else if (shifted) {
                // Function layers: right away, no long press
                a_used = true;
                static constexpr ButtonEvent LAYER_EVENTS[] = {
                    ButtonEvent::FUNCTION, ButtonEvent::FUNCTION2, ButtonEvent::FUNCTION3, ButtonEvent::FUNCTION4};
                push_event(i, LAYER_EVENTS[a_layer - 1]);
            }
        } else if (raw) {
            uint32_t held = now - b.pressed_at;
            if (i == 0 || b.shifted) {
                continue;
            } else if (!b.long_fired && held >= LONG_PRESS_MS) {
                b.long_fired = true;
                b.last_repeat = now;
                push_event(i, ButtonEvent::LONG);
            } else if (b.long_fired && now - b.last_repeat >= REPEAT_MS) {
                b.last_repeat = now;
                push_event(i, ButtonEvent::REPEAT);
            }
        } else if (b.down) {
            b.down = false;
            if (i == 0) {
                if (a_layer < 4 && !a_used && now - b.pressed_at < LONG_PRESS_MS) {
                    tap_pending = true;
                    taps = a_layer;
                    tap_released_at = now;
                }
            } else if (!b.long_fired && !b.shifted) {
                push_event(i, ButtonEvent::PRESS);
            }
        }
    }

    if (tap_pending && now - tap_released_at >= DOUBLE_TAP_MS) {
        tap_pending = false;
        if (taps == 1) push_event(0, ButtonEvent::PRESS);  // several taps alone do nothing
    }

    // The function labels show while A is held for a moment or used as shift key. Layer 2 right away.
    ButtonTracker& a = trackers[0];
    bool layer_shown = a.down && (a_layer >= 2 || a_used || now - a.pressed_at >= LONG_PRESS_MS);
    function_layer = layer_shown ? a_layer : 0;
    return true;
}

void handle_input() {
    while (event_tail != event_head) {
        int button = event_queue[event_tail].button;
        ButtonEvent event = event_queue[event_tail].event;
        event_tail = (event_tail + 1) % EVENT_QUEUE_SIZE;
        initialize_rand();
        last_input_ms = now_ms();

        switch (event) {
            case ButtonEvent::PRESS:
                button_pressed(button);
                break;
            case ButtonEvent::FUNCTION:
                function_pressed(button);
                break;
            case ButtonEvent::FUNCTION2:
                function2_pressed(button);
                break;
            case ButtonEvent::FUNCTION3:
                function3_pressed(button);
                break;
            case ButtonEvent::FUNCTION4:
                function4_pressed(button);
                break;
            case ButtonEvent::LONG:
            case ButtonEvent::REPEAT:
                button_long_pressed(button);
                break;
        }
    }

    if (function_mode != function_layer) {
        function_mode = function_layer;
        state.needs_redraw = true;
    }
}

void initialize_rand() {
    static bool initialized = false;
    if (initialized)
        return;
    srand(to_ms_since_boot(get_absolute_time()));
    initialized = true;
}

Settings current_settings() {
    Settings s{};
    s.center = state.center;
    s.zoom = state.zoom_factor;
    s.palette = static_cast<uint8_t>(color_palette.index());
    s.shading = color_palette.shading;
    s.supersampling = static_cast<uint8_t>(fractalis.supersampling());
    s.color_cycle = static_cast<uint8_t>(color_cycle);
    s.bands = static_cast<uint8_t>(bands);
    s.orbit_trap = static_cast<uint8_t>(fractalis.orbit_trap());
    s.light = static_cast<uint8_t>(light);
    s.hud = hud_enabled;
    s.auto_zoom = state.auto_zoom;
    s.auto_zoom_speed = static_cast<uint8_t>(autoZoom.speed_index());
    s.auto_zoom_pause = static_cast<uint8_t>(autoZoom.pause_index());
    s.auto_zoom_full_quality = state.auto_zoom_full_quality;
    return s;
}

// Invalid values (e.g. from a broken record) fall back to the defaults
void apply_settings(const Settings& s) {
    bool view_valid = std::isfinite(s.center.real.upper) && std::isfinite(s.center.imag.upper)
                      && std::isfinite(s.zoom) && s.zoom > 0;
    if (view_valid)
        fractalis.set_view(s.center, s.zoom);
    color_palette.select(s.palette);
    color_palette.shading = s.shading;
    fractalis.set_supersampling(s.supersampling);
    color_cycle = s.color_cycle < 3 ? static_cast<ColorCycle>(s.color_cycle) : ColorCycle::ALWAYS;
    bands = s.bands < BANDS_COUNT ? s.bands : 0;
    color_palette.set_bands(BANDS[bands]);
    fractalis.set_orbit_trap(s.orbit_trap);
    light = s.light < LIGHT_COUNT ? s.light : 0;
    light_angle = LIGHT_ANGLES[light == LIGHT_ROTATING ? 0 : light];
    color_palette.set_light(light_angle);
    hud_enabled = s.hud;
    autoZoom.set_speed(s.auto_zoom_speed);
    autoZoom.set_pause(s.auto_zoom_pause);
    state.auto_zoom_full_quality = s.auto_zoom_full_quality;
    // A running auto zoom continues where it was
    state.auto_zoom = s.auto_zoom && view_valid;
    if (state.auto_zoom)
        autoZoom.start();
}
