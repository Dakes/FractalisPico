#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio.h"
#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "rgbled.hpp"
#include "button.hpp"
#include "FractalisState.h"
#include <chrono>

#define UPDATE_SLEEP 16
#define LONG_PRESS_DURATION 200/UPDATE_SLEEP

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
    stdio_init_all();
    printf("Starting FractalisPico...\n");

    // Initialize the display
    st7789.set_backlight(255);
    led.set_brightness(100);
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
                calculate_pixel(x, y);

                if (x == 0 && y == 0) {
                    printf("Calculated pixel at (%d, %d): iteration=%d\n", x, y, state.pixelState[y][x].iteration);
                }
            }
        }
        state.rendering = false;
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
    state.rendering = true;  // Set rendering to true initially

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

void calculate_pixel(int x, int y) {
    static uint8_t test = 0;
    test += rand()%2;
    // TODO: Implement Mandelbrot set calculation here
    // This is a placeholder implementation
    // state.pixelState[y][x].iteration = rand() % 256;
    state.pixelState[y][x].iteration = test % 256;
    state.pixelState[y][x].isComplete = true;

    if (x == 0 && y == 0) {
        printf("Calculated pixel at (%d, %d): iteration=%d\n", x, y, state.pixelState[y][x].iteration);
    }
}

void update_display() {
    static uint8_t frame_count = 0;
    frame_count++;

    for(int y = 0; y < state.screen_h; ++y) {
        for(int x = 0; x < state.screen_w; ++x) {

            display.set_pen(display.create_pen_hsv((float)state.pixelState[y][x].iteration / (float)MAX_ITER, 1., 1.));
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
            printf("Button A long press\n");
            state.rendering = true;
            return;
        }
        led.set_rgb(0, 255, 0);
        state.rendering = true;
        printf("Button A pressed\n");
    } else if(button_b.raw()) {
        last_pressed_button = &button_b;
        button_pressed_duration++;
        if (button_pressed_duration > LONG_PRESS_DURATION) {
            led.set_rgb(0, 255, 255);
            printf("Button B long press\n");
            state.rendering = true;
            return;
        }
        state.pan_real -= 0.1 / state.zoom_factor;  // Pan left
        led.set_rgb(255, 0, 0);
        state.rendering = true;
        printf("Button B pressed: Pan left\n");
    } else if(button_x.raw()) {
        last_pressed_button = &button_x;
        button_pressed_duration++;
        state.pan_real += 0.1 / state.zoom_factor;  // Pan right
        led.set_rgb(0, 0, 255);
        state.rendering = true;
        printf("Button X pressed: Pan right\n");
    } else if(button_y.raw()) {  // Zoom in and out
        last_pressed_button = &button_y;
        button_pressed_duration++;
        state.zoom_factor *= 1.1;  // Zoom in
        led.set_rgb(255, 255, 0);
        state.rendering = true;
        printf("Button Y pressed: Zoom in, new zoom_factor=%f\n", state.zoom_factor);
    } else {
        if (last_pressed_button != nullptr) {
            printf("Button released\n");
        }
        button_pressed_duration = 0;
        last_pressed_button = nullptr;
    }
}