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
#define LONG_PRESS_DURATION 200/UPDATE_SLEEP
#define PAN_CONSTANT 0.2
#define ZOOM_CONSTANT 0.2
#define START_HUE 80

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
        if (!state.rendering) {
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
                const float hue = (float)state.pixelState[y][x].iteration / (float)MAX_ITER;
                display.set_pen(display.create_pen_hsv(hue, 1., 1.));
            }
            display.pixel(Point(x, y));
        }
    }
    st7789.update(&display);

}

void handle_input() {
    static uint16_t button_pressed_duration = 0;
    static Button* last_pressed_button = nullptr;
    bool state_changed = false;

    // auto zoom
    if(button_a.raw()) {
        last_pressed_button = &button_a;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {
            led.set_rgb(255, 0, 255);
            return;
        }
        led.set_rgb(0, 255, 0);
    } else if(button_b.raw()) {  // left
        last_pressed_button = &button_b;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {  // down
            state.pan_imag -= PAN_CONSTANT / state.zoom_factor;
            state_changed = true;
            goto handle_input_end;
        }
        state.pan_real -= PAN_CONSTANT / state.zoom_factor;
        state_changed = true;
    } else if(button_x.raw()) {  // right
        last_pressed_button = &button_x;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {  // up
            state.pan_imag += PAN_CONSTANT / state.zoom_factor;
            state_changed = true;
            goto handle_input_end;
        }
        state.pan_real += PAN_CONSTANT / state.zoom_factor;
        state_changed = true;
    } else if(button_y.raw()) {  // Zoom in and out
        last_pressed_button = &button_y;
        if (button_pressed_duration > LONG_PRESS_DURATION) {
            state.zoom_factor /= 1.1;
            state_changed = true;
            goto handle_input_end;
        }
        button_pressed_duration++;
        state.zoom_factor *= 1.1;  // Zoom in
        state_changed = true;
    } else {
        if (last_pressed_button != nullptr) {
            // button released
        }
        button_pressed_duration = 0;
        last_pressed_button = nullptr;
    }
    handle_input_end:
    if (state_changed) {
        if (state.rendering < 2)
            state.rendering++;
    }
}