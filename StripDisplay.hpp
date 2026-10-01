#ifndef STRIP_DISPLAY_H
#define STRIP_DISPLAY_H

#include "libraries/pico_graphics/pico_graphics.hpp"
#include "hardware/spi.h"
#include <cstdint>
#include <cstring>

/**
 * RGB565 graphics without a full frame buffer (saves ~140 KB of RAM). On every display update the image is drawn
 * in strips of a few rows by the draw callback. While one strip is sent to the display by DMA, the next one is
 * drawn into the second buffer.
 *
 * Text and shapes only work inside the draw callback and land in the current strip, everything outside of it is
 * clipped. So the callback draws everything that touches its rows.
 */
class StripDisplay : public pimoroni::PicoGraphics_PenRGB565 {
public:
    static constexpr int STRIP_ROWS = 8;
    // Fills rows first_row .. first_row + rows - 1 into strip (width * rows pixels, display byte order)
    using DrawStrip = void (*)(uint16_t* strip, int first_row, int rows);

    StripDisplay(uint16_t width, uint16_t height, spi_inst_t* spi)
        : PicoGraphics_PenRGB565(width, height, new uint16_t[2 * STRIP_ROWS * width]), spi(spi) {
        buffers[0] = static_cast<uint16_t*>(frame_buffer);
        buffers[1] = buffers[0] + STRIP_ROWS * width;
        // Anything but RGB565, otherwise the ST7789 driver sends frame_buffer as the whole image
        pen_type = PEN_RGB888;
        clip = NOTHING;
    }

    // Without a callback the display is cleared to black
    void set_drawer(DrawStrip drawer) { draw = drawer; }

    void set_pixel(const pimoroni::Point& p) override {
        static_cast<uint16_t*>(frame_buffer)[(p.y - strip_row) * bounds.w + p.x] = color;
    }

    void set_pixel_span(const pimoroni::Point& p, uint l) override {
        uint16_t* buf = static_cast<uint16_t*>(frame_buffer) + (p.y - strip_row) * bounds.w + p.x;
        while (l--) *buf++ = color;
    }

    // Called by the display driver during update(), callback sends the data by DMA
    void frame_convert(PenType, conversion_callback_func callback) override {
        for (int first = 0, n = 0; first < bounds.h; first += STRIP_ROWS, ++n) {
            int rows = first + STRIP_ROWS <= bounds.h ? STRIP_ROWS : bounds.h - first;
            uint16_t* strip = buffers[n & 1];
            // The DMA of the previous strip still reads the other buffer
            frame_buffer = strip;
            strip_row = first;
            clip = pimoroni::Rect(0, first, bounds.w, rows);
            if (draw) {
                draw(strip, first, rows);
            } else {
                memset(strip, 0, rows * bounds.w * sizeof(uint16_t));
            }
            // Waits until the previous strip is out, then starts this one
            callback(strip, rows * bounds.w * sizeof(uint16_t));
        }
        callback(nullptr, 0);  // waits for the DMA
        // The SPI still shifts out the last bytes, the driver deselects the display right after this returns
        if (spi) {
            while (spi_is_busy(spi)) {}
        }
        frame_buffer = buffers[0];
        strip_row = 0;
        clip = NOTHING;
    }

private:
    // Outside of the draw callback there is no strip to draw into
    static inline const pimoroni::Rect NOTHING{0, 0, 0, 0};
    uint16_t* buffers[2];
    spi_inst_t* spi;
    DrawStrip draw = nullptr;
    int strip_row = 0;
};

#endif // STRIP_DISPLAY_H
