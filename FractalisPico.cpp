#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "rgbled.hpp"
#include "button.hpp"
#include "FractalisState.h"
#include <chrono>


#define UPDATE_SLEEP 16
#define LONG_PRESS_DURATION 500/UPDATE_SLEEP

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

// Fractal state
FractalisState state;


// Function prototypes
void core1_entry();
void initialize_state();
void cleanup_state();
void calculate_pixel(int x, int y);
void update_display();
void handle_input();

int main() {
    // Initialize the display
    st7789.set_backlight(255);
    led.set_brightness(100);

    // Initialize the fractal state
    initialize_state();

    // Launch core1
    multicore_launch_core1(core1_entry);

    // Main loop on core0 (display and input handling)
    while(true) {
        update_display();
        handle_input();
        sleep_ms(UPDATE_SLEEP);  // Approximately 60fps
    }

    // Cleanup (this won't be reached in this example, but good practice)
    cleanup_state();

    return 0;
}

// Core1 entry point
void core1_entry() {
    while(true) {
        if (!state.rendering) {
            sleep_ms(16);
            continue;
        }
        // Continuously calculate pixel values
        for(int y = 0; y < state.screen_h; ++y) {
            for(int x = 0; x < state.screen_w; ++x) {
                calculate_pixel(x, y);
            }
        }
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
}

void cleanup_state() {
    // Free allocated memory
    for(int i = 0; i < state.screen_h; ++i) {
        delete[] state.pixelState[i];
    }
    delete[] state.pixelState;
}

void calculate_pixel(int x, int y) {
    // TODO: Implement Mandelbrot set calculation here
    // This is a placeholder implementation
    // set iteration to random number
    // state.pixelState[y][x].iteration = (x + y) % 256;
    state.pixelState[y][x].iteration = rand() % 256;
    state.pixelState[y][x].isComplete = true;
}

void update_display() {
    for(int y = 0; y < state.screen_h; ++y) {
        for(int x = 0; x < state.screen_w; ++x) {
            uint8_t color = state.pixelState[y][x].iteration;
            // display.set_pen(color, color, color);
            display.set_pen(display.create_pen_hsv(state.pixelState[y][x].iteration / MAX_ITER, 1., 1.));
            display.pixel(Point(x, y));
        }
    }
    st7789.update(&display);
}

void handle_input() {
    static uint16_t button_pressed_duration = 0;
    static Button* last_pressed_button = nullptr;

    if(button_a.raw()) {
        last_pressed_button = &button_a;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {
            led.set_rgb(255, 0, 255);
            return;
        }
        led.set_rgb(0, 255, 0);
        // auto zoom
        // long press: disable auto zoom
    } else if(button_b.raw()) {
        last_pressed_button = &button_b;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {
            led.set_rgb(0, 255, 255);
            return;
        }
        state.pan_real -= 0.1 / state.zoom_factor;  // Pan left
        // long press: pan down
        led.set_rgb(255, 0, 0);
    } else if(button_x.raw()) {
        last_pressed_button = &button_x;
        button_pressed_duration++;
        state.pan_real += 0.1 / state.zoom_factor;  // Pan right
        // long press: pan up
        led.set_rgb(0, 0, 255);
    } else if(button_y.raw()) {  // Zoom in and out
        last_pressed_button = &button_y;
        button_pressed_duration++;
        state.zoom_factor *= 1.1;  // Zoom in
        // state.zoom_factor *= 0.9;  // Zoom out
        led.set_rgb(255, 255, 0);
    } else {
        button_pressed_duration = 0;
        last_pressed_button = nullptr;
    }
}