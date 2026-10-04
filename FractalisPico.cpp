#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/unique_id.h"
#include "pico/stdio.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
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
#include "StripDisplay.hpp"
#include "Ui.hpp"
#include <cmath>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

using namespace pimoroni;

// Set to the width and height of your pimoroni display
const uint16_t width = 320;
const uint16_t height = 240;

ST7789 st7789(width, height, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
// RGB565 for smooth color gradients (RGB332 only has 256 colors), drawn in strips instead of a full frame buffer
StripDisplay display(st7789.width, st7789.height, get_spi_pins(BG_SPI_FRONT).spi);
RGBLED led(LED_PIN_R, LED_PIN_G, LED_PIN_B);
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

void core1_entry();
void update_display();
void draw_strip(uint16_t* strip, int first_row, int rows);
void update_led();
void handle_input();
void help_calculating();
bool input_pending();
void initialize_rand();
bool sample_buttons(repeating_timer_t*);

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

extern char __StackLimit;  // end of the heap, set by the linker

// Never touched heap plus freed blocks inside the used heap
unsigned free_ram() {
    char* heap_end = static_cast<char*>(sbrk(0));
    return static_cast<unsigned>(&__StackLimit - heap_end) + mallinfo().fordblks;
}

uint32_t system_clock_khz() {
    return clock_get_hz(clk_sys) / 1000;
}

// SCREEN.BMP on the USB drive. With its own scratch space: it runs while core0 draws a frame, or on core1.
void usb_image_row(int y, uint16_t* row) {
    static PixelState scratch[2 * width];
    color_palette.render_rows(state.pixelState, state.screen_w, state.screen_h, y, 1, row, &state.content, scratch);
    for (int x = 0; x < state.screen_w; ++x) row[x] = static_cast<uint16_t>((row[x] >> 8) | (row[x] << 8));
}

uint32_t usb_serial() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    return id.id[0] | (id.id[1] << 8) | (id.id[2] << 16) | (static_cast<uint32_t>(id.id[3]) << 24);
}

/**
 * Above 200 MHz the flash gets a bigger clock divider (at most 100 MHz flash clock, like at 200 MHz / 2) and read
 * delay. Runs from RAM: the flash is read with the new timing right after.
 */
void __not_in_flash_func(set_flash_timing)(uint32_t sys_khz, uint32_t boot_timing) {
    if (sys_khz <= 200000) {
        qmi_hw->m[0].timing = boot_timing;
        return;
    }
    uint32_t divider = (sys_khz + 99999) / 100000;
    uint32_t timing = boot_timing & ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    qmi_hw->m[0].timing = timing | (divider << QMI_M0_TIMING_CLKDIV_LSB) | (divider << QMI_M0_TIMING_RXDELAY_LSB);
}

/**
 * Above 200 MHz with a slightly higher core voltage (1.15 V instead of 1.10 V, the limit without unlocking is
 * 1.30 V) and a slower flash. The new flash timing comes first, it works with the boot clock as well.
 */
void set_clock() {
    if (SYS_CLOCK_KHZ == 0)
        return;
    if (SYS_CLOCK_KHZ > 200000) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        busy_wait_ms(2);
    }
    const uint32_t boot_timing = qmi_hw->m[0].timing;
    uint32_t interrupts = save_and_disable_interrupts();
    set_flash_timing(SYS_CLOCK_KHZ, boot_timing);
    if (!set_sys_clock_khz(SYS_CLOCK_KHZ, false))
        set_flash_timing(clock_get_hz(clk_sys) / 1000, boot_timing);
    restore_interrupts(interrupts);
    // The peripheral clock follows the system clock, the display SPI needs its baud rate again.
    // The ST7789 requires 16 ns between SPI rising edges = 62.5 MHz
    spi_set_baudrate(get_spi_pins(BG_SPI_FRONT).spi, 62'500'000);
}

// Each core counts its cycles (DWT), for the cycles per iteration in the statistics
void enable_cycle_counter() {
    *reinterpret_cast<volatile uint32_t*>(0xE000EDFC) |= 1u << 24;  // DEMCR.TRCENA
    *reinterpret_cast<volatile uint32_t*>(0xE0001000) |= 1u;        // DWT_CTRL.CYCCNTENA
}

uint32_t read_cycle_counter() {
    return *reinterpret_cast<volatile uint32_t*>(0xE0001004);  // DWT_CYCCNT
}

int main() {
    set_clock();
    // Clear the display RAM before anything else, it is filled with noise after power up.
    // Without a draw callback yet, the display draws black.
    st7789.update(&display);
    st7789.set_backlight(255);
    display.set_drawer(draw_strip);

    // USB: the serial port of the log and the drive with the views (UsbDevice.cpp)
    stdio_init_all();
    if (DEBUG) {
        // Give a serial terminal a moment to connect, so the first messages aren't lost
        uint32_t start = now_ms();
        while (!stdio_usb_connected() && now_ms() - start < USB_WAIT_MS) {
            sleep_ms(10);
        }
    }
    printf("Starting FractalisPico at %lu kHz, display SPI at %lu kHz, flash clock divider %lu, read delay %lu\n",
           static_cast<unsigned long>(clock_get_hz(clk_sys) / 1000),
           static_cast<unsigned long>(spi_get_baudrate(get_spi_pins(BG_SPI_FRONT).spi) / 1000),
           static_cast<unsigned long>((qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB),
           static_cast<unsigned long>((qmi_hw->m[0].timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB));

    led.set_brightness(20);
    printf("Display initialized\n");

    // Statistics per view, for the statistics page of the menu (and printed with DEBUG)
    Fractalis::clock_us = time_us_64;
    enable_cycle_counter();
    Fractalis::cycle_counter = read_cycle_counter;
    // Holding B while powering on starts with the default settings, they are saved once B is released
    bool defaults = button_b.raw();
    ui::start(defaults);
    if (defaults) {
        led.set_rgb(255, 255, 255);
        while (button_b.raw()) sleep_ms(10);  // released before the buttons are sampled, so it doesn't pan
    }
    printf("Fractal state initialized, free RAM: %u KB\n", free_ram() / 1024);

    add_repeating_timer_ms(-BUTTON_SAMPLE_MS, sample_buttons, nullptr, &button_timer);

    multicore_launch_core1(core1_entry);
    printf("Core1 launched\n");

    printf("Entering main loop on core0\n");
    uint32_t last_frame_ms = 0;
    uint32_t last_frame_duration_ms = 0;
    uint32_t seen_passes = 0;
    bool animating = false;
    uint32_t last_loop_ms = now_ms();
    uint32_t seen_calculation = 0;
    uint32_t calculation_started_ms = 0;
    bool was_calculating = false;
    bool was_auto_zoom = false;
    while(true) {
        update_led();
        handle_input();

        uint32_t now = now_ms();
        bool calculating = state.calculating != 0;
        if (state.calculation_id != seen_calculation) {
            seen_calculation = state.calculation_id;
            calculation_started_ms = now;
            ui::view_frames = {};
            // The full coordinates, to find the view again
            char real_text[100], imag_text[100];
            ui::format_coordinate(state.center.real, 74, 1000, real_text, sizeof(real_text));
            ui::format_coordinate(state.center.imag, 74, 1000, imag_text, sizeof(imag_text));
            printf("View at zoom %.6e:\n  real %s\n  imag %s\n", state.zoom_factor, real_text, imag_text);
        }
        if (was_calculating && !calculating) {
            printf("View done in %lu ms (%lu frames drawn, %lu ms drawing)\n",
                   static_cast<unsigned long>(now - calculation_started_ms),
                   static_cast<unsigned long>(ui::view_frames.frames),
                   static_cast<unsigned long>(ui::view_frames.drawing_us / 1000));
        }
        was_calculating = calculating;

        // Auto contrast after every finished pass
        if (state.passes_completed != seen_passes) {
            seen_passes = state.passes_completed;
            color_palette.analyze(state.pixelState, state.screen_w, state.screen_h);
        }
        // Color cycling, the rotating light, timers of the overlay, saving the settings
        if (ui::update(now, now - last_loop_ms)) animating = true;
        last_loop_ms = now;

        // Redraw right away after input, regularly while calculating or animating. While calculating, at most half
        // of core0's time goes to drawing (a frame can take longer than the interval).
        uint32_t since_frame = now - last_frame_ms;
        uint32_t interval = animating ? (calculating ? ANIMATION_INTERVAL_CALCULATING_MS : ANIMATION_INTERVAL_MS)
                                      : FRAME_INTERVAL_MS;
        if (calculating) interval = std::max(interval, 2 * last_frame_duration_ms);
        if (state.needs_redraw || ((animating || calculating) && since_frame >= interval)) {
            last_frame_ms = now;
            animating = color_palette.animate();
            uint64_t drawing_started = time_us_64();
            update_display();
            uint64_t drawing_us = time_us_64() - drawing_started;
            last_frame_duration_ms = static_cast<uint32_t>(drawing_us / 1000);
            if (calculating) {
                ui::view_frames.frames++;
                ui::view_frames.drawing_us += drawing_us;
            }
        }

        // Without full quality, auto zoom doesn't wait for supersampling. The view is supersampled once it stops.
        // It waits for a minibrot search, that may give it a target.
        if (state.auto_zoom && !state.needs_redraw && !ui::searching())
            autoZoom.dive(now_ms());
        if (was_auto_zoom && !state.auto_zoom)
            fractalis.supersample();
        was_auto_zoom = state.auto_zoom;

        if (ui::work(input_pending)) {
            // Core0 searched a minibrot, core1 keeps calculating
        } else if (calculating) {
            help_calculating();
        } else {
            sleep_ms(animating ? 1 : UPDATE_SLEEP);
        }
    }
}

void core1_entry() {
    printf("Core1 started\n");
    enable_cycle_counter();
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

void update_display() {
    state.needs_redraw = false;
    ui::prepare_frame(display);
    st7789.update(&display);  // calls draw_strip() for every strip
}

void draw_strip(uint16_t* strip, int first_row, int rows) {
    color_palette.render_rows(state.pixelState, state.screen_w, state.screen_h, first_row, rows, strip,
                              &state.content);
    ui::draw_strip(display, strip, first_row, rows);
}

void update_led() {
    if (static_cast<int32_t>(now_ms() - led_hold_until_ms) < 0) {
        return;
    }
    uint8_t r, g, b;
    ui::led_color(r, g, b);
    led.set_rgb(r, g, b);
}

void show_led_feedback(uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms) {
    led.set_rgb(r, g, b);
    led_hold_until_ms = now_ms() + duration_ms;
}

// Buttons are sampled by a timer interrupt, so short presses aren't missed while the main loop is busy.
// The interrupt only records the inputs, they are handled in the main loop.
struct InputEvent {
    ui::Input input;
    uint8_t button;
};
constexpr int EVENT_QUEUE_SIZE = 32;
volatile InputEvent event_queue[EVENT_QUEUE_SIZE];
volatile uint8_t event_head = 0;  // written by the interrupt
volatile uint8_t event_tail = 0;  // written by the main loop
void push_event(ui::Input input, int button) {
    uint8_t next = (event_head + 1) % EVENT_QUEUE_SIZE;
    if (next == event_tail) return;  // full, drop it
    event_queue[event_head].input = input;
    event_queue[event_head].button = static_cast<uint8_t>(button);
    event_head = next;
}

/**
 * Exploring:
 *   A: a tap opens the menu. While A is held, B, X and Y have their quick functions: B resets the view, X the next
 *      palette, Y auto zoom on/off.
 *   B, X, Y: a tap pans left/right or zooms in, a long press pans down/up or zooms out and repeats while held.
 *      Tap and then hold (hidden): the tap repeats while held.
 * In the menu: X up, Y down (both repeat while held), A OK (held: the second function of a row, e.g. storing a
 * view), B back (held: closes the menu).
 */
bool sample_buttons(repeating_timer_t*) {
    struct ButtonTracker {
        bool down;
        bool long_fired;
        bool menu;     // pressed while the menu was open: menu inputs until released
        bool shifted;  // pressed while A was held: a quick function
        uint8_t debounce;
        uint32_t pressed_at;
        uint32_t last_repeat;
        bool tap_hold;     // pressed right after a tap: held, it repeats the tap
        uint32_t tap_at;   // when the last tap was released, 0 = the last release wasn't a tap
    };
    static ButtonTracker trackers[4] = {};
    static Button* const buttons[4] = {&button_a, &button_b, &button_x, &button_y};
    static bool a_used = false;  // A was used as shift key, its release doesn't open the menu
    uint32_t now = now_ms();

    for (int i = 0; i < 4; ++i) {
        ButtonTracker& b = trackers[i];
        bool raw = buttons[i]->raw();
        // Debounce: a change only counts once it is stable for a few samples
        if (raw != b.down) {
            if (++b.debounce < BUTTON_DEBOUNCE_SAMPLES) continue;
        }
        b.debounce = 0;
        const bool vertical = i == 2 || i == 3;  // X and Y move the cursor of the menu

        if (raw && !b.down) {
            const bool menu = ui::menu_active;
            const bool shifted = !menu && i != 0 && trackers[0].down && !trackers[0].menu;
            const bool tap_hold = !menu && !shifted && i != 0 && b.tap_at != 0 && now - b.tap_at < TAP_HOLD_MS;
            b = {true, false, menu, shifted, 0, now, now, tap_hold, 0};
            if (i == 0) {
                a_used = false;
            } else if (shifted) {
                a_used = true;
                push_event(ui::Input::QUICK, i);
            } else if (menu && vertical) {
                push_event(i == 2 ? ui::Input::UP : ui::Input::DOWN, i);
            }
        } else if (raw) {
            uint32_t held = now - b.pressed_at;
            if (b.shifted) {
                continue;
            } else if (b.menu) {
                if (vertical) {
                    if (now - b.last_repeat >= (b.long_fired ? MENU_REPEAT_MS : LONG_PRESS_MS)) {
                        b.long_fired = true;
                        b.last_repeat = now;
                        push_event(i == 2 ? ui::Input::UP : ui::Input::DOWN, i);
                    }
                } else if (!b.long_fired && held >= LONG_PRESS_MS) {
                    b.long_fired = true;
                    push_event(i == 0 ? ui::Input::HOLD : ui::Input::CLOSE, i);
                }
            } else if (i == 0) {
                continue;
            } else if (!b.long_fired && held >= LONG_PRESS_MS) {
                b.long_fired = true;
                b.last_repeat = now;
                push_event(b.tap_hold ? ui::Input::PRESS : ui::Input::LONG, i);
            } else if (b.long_fired && now - b.last_repeat >= REPEAT_MS) {
                b.last_repeat = now;
                push_event(b.tap_hold ? ui::Input::PRESS : ui::Input::REPEAT, i);
            }
        } else if (b.down) {
            b.down = false;
            if (b.menu) {
                if (!b.long_fired && !vertical) push_event(i == 0 ? ui::Input::OK : ui::Input::BACK, i);
            } else if (i == 0) {
                if (!a_used && now - b.pressed_at < LONG_PRESS_MS) {
                    // Set right away: the next press already belongs to the menu
                    ui::menu_active = true;
                    push_event(ui::Input::OPEN, 0);
                }
            } else if (!b.long_fired && !b.shifted) {
                push_event(ui::Input::PRESS, i);
                b.tap_at = now;
            }
        }
    }

    // The quick functions show while A is held for a moment or used as shift key
    const ButtonTracker& a = trackers[0];
    ui::quick_shown = a.down && !a.menu && (a_used || now - a.pressed_at >= LONG_PRESS_MS);
    return true;
}

void handle_input() {
    while (event_tail != event_head) {
        ui::Input input = event_queue[event_tail].input;
        int button = event_queue[event_tail].button;
        event_tail = (event_tail + 1) % EVENT_QUEUE_SIZE;
        initialize_rand();
        ui::handle(input, button);
    }
}

void initialize_rand() {
    static bool initialized = false;
    if (initialized)
        return;
    srand(to_ms_since_boot(get_absolute_time()));
    initialized = true;
}
