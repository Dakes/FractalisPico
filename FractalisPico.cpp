#include "pico/stdlib.h"
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

#define UPDATE_SLEEP 16
#define LONG_PRESS_DURATION 100/UPDATE_SLEEP
#define PAN_CONSTANT 0.2
#define ZOOM_CONSTANT 0.2

// #define MAX_ITER 50
constexpr int MAX_ITER = 50;

// defines for color generation
#define START_HUE 0.6222
#define SATURATION_THRESHOLD 0.05f
#define VALUE_THRESHOLD 0.04f

using namespace pimoroni;

// Display driver
ST7789 st7789(PicoDisplay::WIDTH, PicoDisplay::HEIGHT, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));

// Graphics library - in RGB332 mode you get 256 colours and optional dithering for ~32K RAM.
PicoGraphics_PenRGB332 display(st7789.width, st7789.height, nullptr);

// RGB LED
RGBLED led(PicoDisplay::LED_R, PicoDisplay::LED_G, PicoDisplay::LED_B);

// Buttons
Button button_a(PicoDisplay::A);
Button button_b(PicoDisplay::B);
Button button_x(PicoDisplay::X);
Button button_y(PicoDisplay::Y);

FractalisState state;
Fractalis fractalis(&state);


// Function prototypes
void core1_entry();
void initialize_state();
void cleanup_state();
void update_display();
void handle_input();

int main() {
    stdio_init_all();
    printf("Starting FractalisPico...\n");

    // Initialize the display
    st7789.set_backlight(255);
    led.set_brightness(50);
    printf("Display initialized\n");

    // Initialize the fractal state
    initialize_state();
    printf("Fractal state initialized\n");

    // Launch core1
    multicore_launch_core1(core1_entry);
    printf("Core1 launched\n");

    // Main loop on core0 (display and input handling)
    printf("Entering main loop on core0\n");
    while(true) {
        handle_input();
        sleep_ms(UPDATE_SLEEP);
        update_display();
    }

    cleanup_state();

    return 0;
}

// Core1 entry point
void core1_entry() {
    printf("Core1 started\n");
    while(true) {
        if (state.rendering <= 0) {
            sleep_ms(16);
            continue;
        }
        printf("Core1: Calculating pixels\n");
        // Continuously calculate pixel values
        for(int y = 0; y < state.screen_h; ++y) {
            for(int x = 0; x < state.screen_w; ++x) {
                fractalis.calculate_pixel(x, y);
            }
        }
        state.rendering--;
        printf("Core1: Pixel calculation complete\n");
    }
}

void initialize_state() {
    state.screen_w = PicoDisplay::WIDTH;
    state.screen_h = PicoDisplay::HEIGHT;
    
    // Allocate memory for pixelState
    state.pixelState = new PixelState*[state.screen_h];
    for(int i = 0; i < state.screen_h; ++i) {
        state.pixelState[i] = new PixelState[state.screen_w];
    }

    // Initialize other state variables
    state.center = {-0.5, 0};  // Center on the main cardioid
    state.zoom_factor = 1.0;
    state.pan_real = 0;
    state.pan_imag = 0;
    state.rendering = 1;
    fractalis = Fractalis(&state);

    printf("State initialized: screen_w=%d, screen_h=%d, zoom_factor=%f\n", 
           state.screen_w, state.screen_h, state.zoom_factor);
}

void cleanup_state() {
    // Free allocated memory
    for(int i = 0; i < state.screen_h; ++i) {
        delete[] state.pixelState[i];
    }
    delete[] state.pixelState;
    printf("State cleaned up\n");
}

void update_display() {
    static uint8_t frame_count = 0;
    frame_count++;

    for(int y = 0; y < state.screen_h; ++y) {
        for(int x = 0; x < state.screen_w; ++x) {
            if (state.pixelState[y][x].iteration >= MAX_ITER) {
                display.set_pen(0, 0, 0);
            } else {
                float iteration_ratio = (float)state.pixelState[y][x].iteration / (float)MAX_ITER;
                float hue = fmodf(START_HUE + (float)state.pixelState[y][x].iteration / (float)MAX_ITER, 1.0f);
                float saturation = std::min(iteration_ratio / SATURATION_THRESHOLD, 1.0f);
                float value = std::min(iteration_ratio / VALUE_THRESHOLD, 1.0f);
                display.set_pen(display.create_pen_hsv(hue, saturation, value));
            }
            display.pixel(Point(x, y));
        }
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
                // Handle long press actions
                switch (i) {
                    case 0: // Button A
                        led.set_rgb(255, 0, 255);
                        break;
                    case 1: // Button B
                        state.pan_imag += PAN_CONSTANT / state.zoom_factor;
                        state_changed = true;
                        break;
                    case 2: // Button X
                        state.pan_imag -= PAN_CONSTANT / state.zoom_factor;
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        state.zoom_factor /= (1 + ZOOM_CONSTANT);
                        state_changed = true;
                        break;
                }
            } else if (button_states[i] == ButtonState::LONG_PRESSED) {
                button_states[i] = ButtonState::HELD;
            }
        } else if (button_states[i] != ButtonState::IDLE) {
            // Button just released
            if (button_states[i] == ButtonState::PRESSED) {
                // Handle short press actions
                switch (i) {
                    case 0: // Button A
                        led.set_rgb(0, 255, 0);
                        break;
                    case 1: // Button B
                        state.pan_real -= PAN_CONSTANT / state.zoom_factor;
                        state_changed = true;
                        break;
                    case 2: // Button X
                        state.pan_real += PAN_CONSTANT / state.zoom_factor;
                        state_changed = true;
                        break;
                    case 3: // Button Y
                        state.zoom_factor *= (1 + ZOOM_CONSTANT);
                        state_changed = true;
                        break;
                }
            }
            button_states[i] = ButtonState::IDLE;
            button_pressed_durations[i] = 0;
        }
    }

    if (state_changed && state.rendering < 2) {
        state.rendering++;
    }
}