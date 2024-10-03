#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/stdio.h"
#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "libraries/bitmap_fonts/bitmap_fonts.hpp"
#include "rgbled.hpp"
#include "button.hpp"
#include "FractalisState.h"
#include "fractalis.h"
#include "globals.h"
#include "doubledouble.h"
#include <chrono>
#include <cmath>

using namespace doubledouble;

#define DEBUG true
#define UPDATE_SLEEP 16
#define LONG_PRESS_DURATION 100/UPDATE_SLEEP
#define PAN_CONSTANT 0.2L
#define ZOOM_CONSTANT 0.2L
#define UPDATE_INTERVAL 100  // Update display every 100 pixels calculated

#define LOWEST_ITER 25
int INITIAL_ITER = 25;
int MAX_ITER = 255;

#define START_HUE 0.6222
#define SATURATION_THRESHOLD 0.05f
#define VALUE_THRESHOLD 0.04f

using namespace pimoroni;

ST7789 st7789(PicoDisplay::WIDTH, PicoDisplay::HEIGHT, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
PicoGraphics_PenRGB332 display(st7789.width, st7789.height, nullptr);
RGBLED led(PicoDisplay::LED_R, PicoDisplay::LED_G, PicoDisplay::LED_B);
Button button_a(PicoDisplay::A);
Button button_b(PicoDisplay::B);
Button button_x(PicoDisplay::X);
Button button_y(PicoDisplay::Y);

FractalisState state(PicoDisplay::WIDTH, PicoDisplay::HEIGHT);
Fractalis fractalis(&state);

void core1_entry();
void initialize_state();
void cleanup_state();
void update_display();
void update_led();
void update_iter_limit();
void render_fractal();
void render_overlay();
void handle_input();
void calculate_pixel_concentric(int x, int y);

int main() {
    if (DEBUG) {
        stdio_init_all();
        uint16_t time = 0;
        while (!stdio_usb_connected()) {
            time++;
            if (time >= 200)
                break;
            sleep_ms(100);
        }
    }
    printf("Starting FractalisPico...\n");

    st7789.set_backlight(255);
    led.set_brightness(20);
    printf("Display initialized\n");

    initialize_state();
    printf("Fractal state initialized\n");

    multicore_launch_core1(core1_entry);
    printf("Core1 launched\n");

    printf("Entering main loop on core0\n");
    while(true) {
        update_led();
        handle_input();
        update_display();
        sleep_ms(UPDATE_SLEEP);
    }

    cleanup_state();
    return 0;
}

void initialize_state() {
    state.calculating = 2;
    state.rendering = 2;
    state.iteration_limit = INITIAL_ITER;
    state.zoom_factor = DoubleDouble(1.0);
    state.pan_real = DoubleDouble(0.0);
    state.pan_imag = DoubleDouble(0.0);

    printf("State initialized: screen_w=%d, screen_h=%d, zoom_factor=%.15f\n", 
           state.screen_w, state.screen_h, state.zoom_factor.upper + state.zoom_factor.lower);
}

void core1_entry() {
    printf("Core1 started\n");
    int center_x = state.screen_w / 2;
    int center_y = state.screen_h / 2;
    int max_radius = std::max(center_x, center_y);
    
    while(true) {
        if (state.calculating <= 0) {
            sleep_ms(UPDATE_SLEEP);
            continue;
        }
        if (state.skip_pre_render) {
            state.calculating = 1;
        }

        uint8_t current_calculation_id = state.calculation_id;
        uint16_t pixels_calculated = 0;
        for(int radius = 0; radius <= max_radius; ++radius) {
            if (state.calculation_id != current_calculation_id) {
                printf("Calculation interrupted at radius %d, restarting\n", radius);
                if (!state.skip_pre_render) {
                    state.calculating = 2;
                    state.iteration_limit = INITIAL_ITER;
                }
                break;
            }
            for(int x = -radius; x <= radius; ++x) {
                fractalis.calculate_pixel(center_x + x, center_y + radius, state.iteration_limit);
                fractalis.calculate_pixel(center_x + x, center_y - radius, state.iteration_limit);
                pixels_calculated += 2;
            }
            for(int y = -radius + 1; y < radius; ++y) {
                fractalis.calculate_pixel(center_x + radius, center_y + y, state.iteration_limit);
                fractalis.calculate_pixel(center_x - radius, center_y + y, state.iteration_limit);
                pixels_calculated += 2;
            }

            if (pixels_calculated >= UPDATE_INTERVAL) {
                state.last_updated_radius = radius;
                pixels_calculated = 0;
            }
        }

        printf("Core1: Pixel calculation complete for iteration limit %d\n", state.iteration_limit);
        if (state.iteration_limit == INITIAL_ITER && state.calculation_id == current_calculation_id && !state.skip_pre_render) {
            state.iteration_limit = MAX_ITER;
            state.resetPixelComplete();
            state.calculating = 1;
        } else if (state.calculation_id == current_calculation_id) {
            state.rendering = 2;
            state.calculating = 0;
        }
    }
}

void cleanup_state() {
    for(int i = 0; i < state.screen_h; ++i) {
        delete[] state.pixelState[i];
    }
    delete[] state.pixelState;
    printf("State cleaned up\n");
}

void update_display() {
    if (state.rendering <= 0)
        return;
    if (state.rendering == 2)
        render_fractal();

    render_overlay();

    // Update the display after rendering the fractal and overlay
    st7789.update(&display);
}

void render_fractal() {
    const int last_updated = state.last_updated_radius;

    const int center_x = state.screen_w / 2;
    const int center_y = state.screen_h / 2;
    uint16_t pixel_rendered_counter = 0;

    for(int y = std::max(0, center_y - last_updated); y < std::min(state.screen_h, center_y + last_updated + 1); ++y) {
        for(int x = std::max(0, center_x - last_updated); x < std::min(state.screen_w, center_x + last_updated + 1); ++x) {
            if (!state.pixelState[y][x].isComplete)
                continue;
            if (state.pixelState[y][x].iteration >= state.iteration_limit) {
                display.set_pen(0, 0, 0);
            } else {
                float iteration_ratio = std::log(1 + state.pixelState[y][x].smooth_iteration) / 2.0f;
                float hue = fmodf(START_HUE + iteration_ratio, 1.0f);
                float saturation = std::min(iteration_ratio / SATURATION_THRESHOLD, 1.0f);
                float value = std::min(iteration_ratio / VALUE_THRESHOLD, 1.0f);
                display.set_pen(display.create_pen_hsv(hue, saturation, value));
            }
            pixel_rendered_counter++;
            display.pixel(Point(x, y));
        }
    }

    if (pixel_rendered_counter >= state.screen_w * state.screen_h) {
        state.rendering = 1;
        printf("setting state.rendering to 1\n");
    }
}

void render_overlay() {
    if (state.hide_ui)
        return;

    // Set pen color to white
    display.set_pen(255, 255, 255);

    // Set font to font6 for button labels
    // Using the bitmap fonts from your dependencies
    display.set_font(&font6);

    int scale = 1;

    // Get font height
    int font6_height = font6.height * scale;

    // Button functionalities
    const char* text_a = "Function";
    const char* text_b = "Pan Left/Down";
    const char* text_x = "Pan Right/Up";
    const char* text_y = "Zoom";

    // Measure text widths
    int32_t text_a_width = display.measure_text(text_a, scale, 1);
    int32_t text_b_width = display.measure_text(text_b, scale, 1);
    int32_t text_x_width = display.measure_text(text_x, scale, 1);
    int32_t text_y_width = display.measure_text(text_y, scale, 1);

    int margin = 5;

    // Positions
    display.text(text_a, Point(margin, margin), display.bounds.w, scale);
    display.text(text_b, Point(margin, display.bounds.h - font6_height - margin), display.bounds.w, scale);
    display.text(text_x, Point(display.bounds.w - text_b_width - margin, margin), display.bounds.w, scale);
    display.text(text_y, Point(display.bounds.w - text_y_width - margin, display.bounds.h - font6_height - margin), display.bounds.w, scale);

    // Display coordinates and zoom factor
    // Use font8 for better readability
    display.set_font(&font8);

    int font8_height = font8.height * scale;

    scale = 1;

    // Compute effective center coordinates
    DoubleDouble effective_center_real = state.center.real + state.pan_real;
    DoubleDouble effective_center_imag = state.center.imag + state.pan_imag;

    // Coordinates text
    char coord_text[100];
    snprintf(coord_text, sizeof(coord_text), "Coordinates:\n%.10f\n%.10f", 
             effective_center_real.upper + effective_center_real.lower,
             effective_center_imag.upper + effective_center_imag.lower);
    int32_t coord_text_width = display.measure_text(coord_text, scale, 1);

    // Zoom factor text
    char zoom_text[30];
    snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.2f", state.zoom_factor.upper + state.zoom_factor.lower);
    int32_t zoom_text_width = display.measure_text(zoom_text, scale, 1);

    // Position for coordinates and zoom factor
    int info_y = margin*2 + margin + font8_height;
    display.text(coord_text, Point(margin, info_y), display.bounds.w - 2 * margin, scale);

    info_y += font8_height*3 + margin;
    display.text(zoom_text, Point(margin, info_y), display.bounds.w - 2 * margin, scale);
}

void update_led() {
    if (state.led_skip_counter > 0) {
        state.led_skip_counter--;
        return;
    }

    if (state.calculating == 2) {
        led.set_rgb(255, 25, 0);
    } else if (state.calculating == 1) {
        led.set_rgb(255, 200, 0);
    } else if (state.calculating == 0 && state.rendering <= 1) {
        led.set_rgb(0, 255, 0);
    }
}

void update_iter_limit() {
    if (state.zoom_factor >= DoubleDouble(1e8)) {
        state.skip_pre_render = true;
        INITIAL_ITER = MAX_ITER;
        return;
    }
    const DoubleDouble log_zoom = state.zoom_factor.log() / dd_ln2 / DoubleDouble(3.321928094887362); // log10(2)
    const DoubleDouble min_log_zoom(0);
    const DoubleDouble max_log_zoom(9);

    const DoubleDouble normalized_zoom = (log_zoom - min_log_zoom) / (max_log_zoom - min_log_zoom);

    // Apply a power function for more aggressive scaling. Bigger number, slower scaling
    const DoubleDouble power_curve = normalized_zoom.powi(2);

    // Apply sigmoid function to smooth out the extremes
    const DoubleDouble sigmoid = DoubleDouble(1) / (DoubleDouble(1) + (-DoubleDouble(12) * (power_curve - DoubleDouble(0.5))).exp());

    // Map sigmoid output to iteration range
    const int min_iter = LOWEST_ITER;
    INITIAL_ITER = min_iter + static_cast<int>((MAX_ITER - min_iter) * sigmoid.upper);
    printf("new initial iter: %d\n", INITIAL_ITER);

    if (INITIAL_ITER > 150) {
        state.skip_pre_render = true;
        INITIAL_ITER = MAX_ITER;
    } else {
        state.skip_pre_render = false;
    }
}

void handle_input() {
    enum class ButtonState { IDLE, PRESSED, LONG_PRESSED, HELD };
    static ButtonState button_states[4] = {ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE};
    static uint16_t button_pressed_durations[4] = {0, 0, 0, 0};
    
    Button* buttons[4] = {&button_a, &button_b, &button_x, &button_y};
    bool state_changed = false;
    ButtonState new_state = ButtonState::IDLE;

    for (int i = 0; i < 4; ++i) {
        if (buttons[i]->raw()) {
            if (button_states[i] == ButtonState::IDLE) {
                button_states[i] = ButtonState::PRESSED;
                button_pressed_durations[i] = 0;
            }
            button_pressed_durations[i]++;
            
            if (button_pressed_durations[i] > LONG_PRESS_DURATION && button_states[i] == ButtonState::PRESSED) {
                button_states[i] = ButtonState::LONG_PRESSED;
                new_state = ButtonState::LONG_PRESSED;
                switch (i) {
                    case 0: // Button A: Function
                        state.hide_ui = !state.hide_ui;
                        // TODO: auto zoom start/stop
                        // TODO: reset to initial position
                        if (button_pressed_durations[i] > LONG_PRESS_DURATION*4) {
                            printf("Longer function press");
                            led.set_rgb(50, 100, 150);
                        } else if (button_pressed_durations[i] > LONG_PRESS_DURATION*8 ) {
                            led.set_rgb(150, 100, 50);
                            printf("Very long function press");
                        }
                        led.set_rgb(255, 0, 255);
                        state_changed = true;
                        break;
                    case 1: // Button B
                        fractalis.pan(DoubleDouble(0), DoubleDouble(PAN_CONSTANT));
                        state_changed = true;
                        break;
                    case 2: // Button X
                        fractalis.pan(DoubleDouble(0), DoubleDouble(-PAN_CONSTANT));
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        fractalis.zoom(DoubleDouble(-ZOOM_CONSTANT));
                        state_changed = true;
                        update_iter_limit();
                        break;
                }
            } else if (button_states[i] == ButtonState::LONG_PRESSED) {
                button_states[i] = ButtonState::HELD;
            }
        } else if (button_states[i] != ButtonState::IDLE) {
            if (button_states[i] == ButtonState::PRESSED) {
                new_state = ButtonState::PRESSED;
                switch (i) {
                    case 0: // Button A
                        led.set_rgb(0, 255, 0);
                        break;
                    case 1: // Button B
                        fractalis.pan(DoubleDouble(-PAN_CONSTANT), DoubleDouble(0));
                        state_changed = true;
                        break;
                    case 2: // Button X
                        fractalis.pan(DoubleDouble(PAN_CONSTANT), DoubleDouble(0));
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        fractalis.zoom(DoubleDouble(ZOOM_CONSTANT));
                        state_changed = true;
                        update_iter_limit();
                        break;
                }
            }
            button_states[i] = ButtonState::IDLE;
            button_pressed_durations[i] = 0;
        }
    }

    if (new_state == ButtonState::PRESSED) {
        led.set_rgb(0, 0, 255);
        state.led_skip_counter = 3;
    } else if (new_state == ButtonState::LONG_PRESSED) {
        led.set_rgb(200, 0, 255);
        state.led_skip_counter = 7;
    }

    if (state_changed) {
        state.rendering = 2;
        state.calculating = 2;
        state.calculation_id++;
        state.last_updated_radius = 0;
        state.iteration_limit = INITIAL_ITER;
        state.resetPixelComplete();
    }
}