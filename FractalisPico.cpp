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
#include "AutoZoom.hpp"
#include "globals.h"
#include "doubledouble.h"
#include <chrono>
#include <cmath>

using namespace pimoroni;
using namespace doubledouble;


ST7789 st7789(PicoDisplay::WIDTH, PicoDisplay::HEIGHT, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
PicoGraphics_PenRGB332 display(st7789.width, st7789.height, nullptr);
RGBLED led(PicoDisplay::LED_R, PicoDisplay::LED_G, PicoDisplay::LED_B);
Button button_a(PicoDisplay::A);
Button button_b(PicoDisplay::B);
Button button_x(PicoDisplay::X);
Button button_y(PicoDisplay::Y);

FractalisState state(PicoDisplay::WIDTH, PicoDisplay::HEIGHT);
Fractalis fractalis(&state);
AutoZoom autoZoom(&state, &fractalis);

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
void initialize_rand();


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
        if (state.auto_zoom && state.calculating <= 0 && state.rendering <= 0)
            autoZoom.dive();
        sleep_ms(UPDATE_SLEEP);
    }

    cleanup_state();
    return 0;
}

void initialize_state() {
    state.calculating = 2;
    state.rendering = 2;

    printf("State initialized: screen_w=%d, screen_h=%d, zoom_factor=%f\n", 
           state.screen_w, state.screen_h, state.zoom_factor);
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

        update_iter_limit();

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

        printf("Core1: Pixel calculation complete for iteration limit: %d. Pre-render: %d\n", state.iteration_limit, !state.skip_pre_render);
        if (state.calculation_id == current_calculation_id) {
            if (!state.skip_pre_render && state.calculating >= 2) {
                state.resetPixelComplete();
                state.rendering = 3;  // Trigger a full render
                state.calculating = 1;
            } else if (state.calculating == 1) {
                state.rendering = 3;  // Trigger a full render
                state.calculating = 0;
            }
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
    if (state.rendering > 0)
        render_fractal();

    render_overlay();

    // Update the display after rendering the fractal and overlay
    st7789.update(&display);
}

void render_fractal() {
    // TODO: this must be determined dynamically but maybe we don't need it at all??
    static const uint16_t PIXEL_SHIFT_X = static_cast<int>(PAN_CONSTANT * state.screen_w / 3.0);
    static const uint16_t PIXEL_SHIFT_Y = static_cast<int>(PAN_CONSTANT * state.screen_h / 2.0);
    
    int start_x = 0, end_x = state.screen_w;
    int start_y = 0, end_y = state.screen_h;
    
    if (state.rendering == 2) {
        if (state.last_pan_direction == PAN_LEFT) {
            start_x = 0;
            end_x = PIXEL_SHIFT_X;
        } else if (state.last_pan_direction == PAN_RIGHT) {
            start_x = state.screen_w - PIXEL_SHIFT_X;
            end_x = state.screen_w;
        } else if (state.last_pan_direction == PAN_UP) {
            start_y = 0;
            end_y = PIXEL_SHIFT_Y;
        } else if (state.last_pan_direction == PAN_DOWN) {
            start_y = state.screen_h - PIXEL_SHIFT_Y;
            end_y = state.screen_h;
        }
    }
    // For state.rendering == 3, we'll render the full screen

    uint32_t pixel_rendered_counter = 0;
    uint32_t total_pixels = (end_x - start_x) * (end_y - start_y);

    for(int y = start_y; y < end_y; ++y) {
        for(int x = start_x; x < end_x; ++x) {
            if (!state.pixelState[y][x].isComplete) {
                continue;
            } else if (state.pixelState[y][x].iteration >= state.iteration_limit) {
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

    if (pixel_rendered_counter >= total_pixels) {
        if (state.rendering == 3) {
            state.rendering = 2;  // Set to 2 to indicate partial renders are now possible
        } else {
            state.rendering = 0;
            state.last_pan_direction = PAN_NONE;
        }
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
    int font_height = font6.height * scale;

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
    display.text(text_b, Point(margin, display.bounds.h - font_height - margin), display.bounds.w, scale);
    display.text(text_x, Point(display.bounds.w - text_b_width - margin, margin), display.bounds.w, scale);
    display.text(text_y, Point(display.bounds.w - text_y_width - margin, display.bounds.h - font_height - margin), display.bounds.w, scale);

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
    if (state.zoom_factor < 1e3)
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.2f", state.zoom_factor);
    else
        snprintf(zoom_text, sizeof(zoom_text), "Zoom: x%.1e", state.zoom_factor);
    int32_t zoom_text_width = display.measure_text(zoom_text, scale, 1);

    // Position for coordinates and zoom factor
    int info_y = margin*2 + margin + font8_height;
    display.text(coord_text, Point(margin, info_y), display.bounds.w - 2 * margin, scale);

    info_y += font8_height*3 + margin;
    display.text(zoom_text, Point(margin, info_y), display.bounds.w - 2 * margin, scale);

    if (state.auto_zoom) {
        info_y += font8_height + margin;
        display.text("Auto Zoom: ON", Point(margin, info_y), display.bounds.w, scale);
    }
}

void update_led() {
    if (state.led_skip_counter > 0) {
        state.led_skip_counter--;
        return;
    }

    if (state.calculating == 2) {
        led.set_rgb(255, 10, 0);
    } else if (state.calculating == 1) {
        led.set_rgb(255, 150, 0);
    } else if (state.calculating == 0 && state.rendering <= 1) {
        if (state.auto_zoom)
            led.set_rgb(0, 255, 150);
        else
            led.set_rgb(0, 255, 0);
    }
}

void update_iter_limit() {
    if (state.zoom_factor > 1e6)
        state.skip_pre_render = true;
    else
        state.skip_pre_render = false;

    if (state.calculating == 0)
        return;
    double scale = state.screen_w / (3.0 / state.zoom_factor);
    int max_iter = static_cast<int>(50 * std::pow(std::log10(scale), 1.25));
    if (max_iter > MAX_ITER) {
        max_iter = MAX_ITER;
    }

    if (state.calculating == 1 || state.skip_pre_render) {
        state.iteration_limit = max_iter;
    } else if (state.calculating == 2 && !state.skip_pre_render) {
        int divider = 6;
        if (state.zoom_factor > 1e4)
            divider -= 1;
        if (state.zoom_factor > 1e5)
            divider -= 1;
            
        state.iteration_limit = max_iter / divider;
    }
    printf("New iteration limit: %d\n", state.iteration_limit);
}

void handle_input() {
    enum class ButtonState { IDLE, PRESSED, LONG_PRESSED, HELD };
    static ButtonState button_states[4] = {ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE};
    static uint16_t button_pressed_durations[4] = {0, 0, 0, 0};
    
    Button* buttons[4] = {&button_a, &button_b, &button_x, &button_y};
    bool state_changed = false;
    ButtonState new_state = ButtonState::IDLE;

    initialize_rand();
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
                        // TODO: reset to initial position
                        if (button_pressed_durations[i] > LONG_PRESS_DURATION*4) {
                            printf("Longer function press");
                            led.set_rgb(50, 100, 150);
                        } else if (button_pressed_durations[i] > LONG_PRESS_DURATION * 8) {
                            led.set_rgb(150, 100, 50);
                            printf("Very long function press");
                        }
                        led.set_rgb(255, 0, 255);
                        break;
                    case 1: // Button B: Pan Down
                        fractalis.pan(0, PAN_CONSTANT);
                        break;
                    case 2: // Button X: Pan Up
                        fractalis.pan(0, -PAN_CONSTANT);
                        break;
                    case 3: // Button Y: Zoom
                        fractalis.zoom(-ZOOM_CONSTANT);
                        state.resetPixelComplete();
                        state_changed = true;
                        break;
                }
            } else if (button_states[i] == ButtonState::LONG_PRESSED) {
                button_states[i] = ButtonState::HELD;
            }
        } else if (button_states[i] != ButtonState::IDLE) {
            if (button_states[i] == ButtonState::PRESSED) {
                new_state = ButtonState::PRESSED;
                switch (i) {
                    case 0: // Button A: Auto Zoom
                        state.auto_zoom = !state.auto_zoom;
                        break;
                    case 1: // Button B: Pan Left
                        fractalis.pan(-PAN_CONSTANT, 0);
                        break;
                    case 2: // Button X: Pan Right
                        fractalis.pan(PAN_CONSTANT, 0);
                        break;
                    case 3: // Button Y: Zoom
                        state_changed = true;
                        fractalis.zoom(ZOOM_CONSTANT);
                        state.resetPixelComplete();
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
        state.calculating = 2;
        state.calculation_id++;
        state.last_updated_radius = 0;
        state.rendering = 3;
    }
}

void initialize_rand() {
    static bool initialized = false;
    if (initialized)
        return;
    srand(to_ms_since_boot(get_absolute_time()));
    initialized = true;
}