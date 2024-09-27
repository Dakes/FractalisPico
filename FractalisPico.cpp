#include "pico_display.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "rgbled.hpp"
#include "button.hpp"

using namespace pimoroni;

// Display driver
ST7789 st7789(PicoDisplay::WIDTH, PicoDisplay::HEIGHT, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));

// Graphics library - in RGB332 mode you get 256 colours and optional dithering for ~32K RAM.
PicoGraphics_PenRGB332 graphics(st7789.width, st7789.height, nullptr);

// RGB LED
RGBLED led(PicoDisplay::LED_R, PicoDisplay::LED_G, PicoDisplay::LED_B);

// And each button
Button button_a(PicoDisplay::A);
Button button_b(PicoDisplay::B);
Button button_x(PicoDisplay::X);
Button button_y(PicoDisplay::Y);

int main() {
    led.set_brightness(100);

    // set the backlight to a value between 0 and 255
    // the backlight is driven via PWM and is gamma corrected by our
    // library to give a gorgeous linear brightness range.
    st7789.set_backlight(255);

    while(true) {
        // detect if the A button is pressed (could be A, B, X, or Y)
        if(button_a.raw()) {
            // make the led glow green
            // parameters are red, green, blue all between 0 and 255
            // these are also gamma corrected
            led.set_rgb(0, 255, 0);
        } else if (button_b.raw()) {
            led.set_rgb(255, 0, 0);
        } else if (button_x.raw()) {
            led.set_rgb(0, 0, 255);
        } else if (button_y.raw()) {
            led.set_rgb(255, 255, 0);
        }


        // set the colour of the pen
        // parameters are red, green, blue all between 0 and 255
        graphics.set_pen(10, 255, 10);

        // fill the screen with the current pen colour
        graphics.clear();

        // draw a box to put some text in
        graphics.set_pen(10, 20, 30);
        Rect text_rect(10, 10, 150, 150);
        graphics.rectangle(text_rect);

        // write some text inside the box with 10 pixels of margin
        // automatically word wrapping
        text_rect.deflate(10);
        graphics.set_pen(110, 120, 130);
        graphics.text("This is a message of doom", Point(text_rect.x, text_rect.y), text_rect.w);

        // now we've done our drawing let's update the screen
        st7789.update(&graphics);
    }
}