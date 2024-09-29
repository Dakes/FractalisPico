#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "pico/stdio.h"
#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "rgbled.hpp"
#include "button.hpp"
#include "FractalisState.h"
#include "fractalis.h"
#include <chrono>
#include <cmath>

#define DEBUG true
#define UPDATE_SLEEP 16
#define LONG_PRESS_DURATION 100/UPDATE_SLEEP
#define PAN_CONSTANT 0.2L
#define ZOOM_CONSTANT 0.2L
#define UPDATE_INTERVAL 100  // Update display every 100 pixels calculated

extern const int INITIAL_ITER = 50;
extern const int MAX_ITER = 255;

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
void render_fractal();
void handle_input();
void calculate_pixel_concentric(int x, int y);

int main() {
    if (DEBUG) {
        stdio_init_all();
        while (!stdio_usb_connected()) {
            sleep_ms(100);
        }
    }
    printf("Starting FractalisPico...\n");

    st7789.set_backlight(255);
    led.set_brightness(50);
    printf("Display initialized\n");

    initialize_state();
    printf("Fractal state initialized\n");

    multicore_launch_core1(core1_entry);
    printf("Core1 launched\n");

    printf("Entering main loop on core0\n");
    while(true) {
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
    // fractalis = Fractalis(&state);

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

        uint8_t current_calculation_id = state.calculation_id;
        uint16_t pixels_calculated = 0;
        for(int radius = 0; radius <= max_radius; ++radius) {
            if (state.calculation_id != current_calculation_id) {
                printf("Calculation interrupted at radius %d, restarting\n", radius);
                state.calculating = 2;
                state.iteration_limit = INITIAL_ITER;
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
        if (state.iteration_limit == INITIAL_ITER && state.calculation_id == current_calculation_id) {
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

    // state.rendering--;
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
                float iteration_ratio = (float)state.pixelState[y][x].iteration / (float)state.color_iteration_limit;
                float hue = fmodf(START_HUE + iteration_ratio, 1.0f);
                float saturation = std::min(iteration_ratio / SATURATION_THRESHOLD, 1.0f);
                float value = std::min(iteration_ratio / VALUE_THRESHOLD, 1.0f);
                display.set_pen(display.create_pen_hsv(hue, saturation, value));
            }
            pixel_rendered_counter++;
            display.pixel(Point(x, y));
        }
    }
    if (pixel_rendered_counter >= state.screen_w*state.screen_h) {
        state.rendering = 1;
        printf("setting state.rendering to 1\n");
    }
    st7789.update(&display);
}


void handle_input() {
    enum class ButtonState { IDLE, PRESSED, LONG_PRESSED, HELD };
    static ButtonState button_states[4] = {ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE, ButtonState::IDLE};
    static uint16_t button_pressed_durations[4] = {0, 0, 0, 0};
    
    Button* buttons[4] = {&button_a, &button_b, &button_x, &button_y};
    bool state_changed = false;

    for (int i = 0; i < 4; ++i) {
        if (buttons[i]->raw()) {
            if (button_states[i] == ButtonState::IDLE) {
                button_states[i] = ButtonState::PRESSED;
                button_pressed_durations[i] = 0;
            }
            button_pressed_durations[i]++;
            
            if (button_pressed_durations[i] > LONG_PRESS_DURATION && button_states[i] == ButtonState::PRESSED) {
                button_states[i] = ButtonState::LONG_PRESSED;
                switch (i) {
                    case 0: // Button A
                        led.set_rgb(255, 0, 255);
                        break;
                    case 1: // Button B
                        fractalis.pan(0, PAN_CONSTANT);
                        state_changed = true;
                        break;
                    case 2: // Button X
                        fractalis.pan(0, -PAN_CONSTANT);
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        fractalis.zoom(-ZOOM_CONSTANT);
                        state_changed = true;
                        break;
                }
            } else if (button_states[i] == ButtonState::LONG_PRESSED) {
                button_states[i] = ButtonState::HELD;
            }
        } else if (button_states[i] != ButtonState::IDLE) {
            if (button_states[i] == ButtonState::PRESSED) {
                switch (i) {
                    case 0: // Button A
                        led.set_rgb(0, 255, 0);
                        break;
                    case 1: // Button B
                        fractalis.pan(-PAN_CONSTANT, 0.L);
                        state_changed = true;
                        break;
                    case 2: // Button X
                        fractalis.pan(PAN_CONSTANT, 0.L);
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        fractalis.zoom(ZOOM_CONSTANT);
                        state_changed = true;
                        break;
                }
            }
            button_states[i] = ButtonState::IDLE;
            button_pressed_durations[i] = 0;
        }
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