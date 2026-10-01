#ifndef FRACTALIS_STATE_H
#define FRACTALIS_STATE_H

#include <cstdint>
#include "doubledouble.h"

using namespace doubledouble;

// Real and imaginary coordinates in the Mandelbrot fractal
struct Coordinate {
    DoubleDouble real;
    DoubleDouble imag;
};

// Per-pixel result of the calculation
struct PixelState {
    enum Flags : uint8_t {
        COMPLETE = 1 << 0,  // calculated with the current view and iteration limit
        IN_SET   = 1 << 1,  // did not escape within the iteration limit
        VALID    = 1 << 2,  // has something to display (also true for previews after zoom/pre-render)
        PREVIEW_COLOR = 1 << 3,  // in the set at the pre-render limit, but still shows the color of the preview
    };

    // palette position, upper 16 and lower 8 bit, see palette.h
    uint16_t color;
    uint8_t flags;
    uint8_t fine;

    bool isComplete() const { return flags & COMPLETE; }
    bool isInSet() const { return flags & IN_SET; }
    bool isValid() const { return flags & VALID; }
    // Escaped, with a final or preview value
    bool hasPosition() const { return isValid() && !isInSet(); }
    // Drawn in color (not black)
    bool showsColor() const { return isValid() && (!isInSet() || (flags & PREVIEW_COLOR)); }

    uint32_t position() const { return (static_cast<uint32_t>(color) << 8) | fine; }
    void setPosition(uint32_t position) {
        color = static_cast<uint16_t>(position >> 8);
        fine = static_cast<uint8_t>(position & 0xFF);
    }

    // Keep the displayed value, but mark the pixel for recalculation
    void markIncomplete() { flags &= ~COMPLETE; }
    void clear() { color = 0; flags = 0; fine = 0; }
};

class FractalisState {
public:

    FractalisState(int width, int height);
    ~FractalisState();

    void resetPixelComplete();
    // Moves the pixel content by dx/dy pixels. Uncovered pixels are cleared.
    void shiftPixelState(int dx, int dy);
    /**
     * Rescales the pixel content around the screen center as a preview for a zoom.
     * ratio = old pixel size / new pixel size. All pixels are marked incomplete.
     */
    void scalePixelState(double ratio);

    // Public members
    int screen_w;
    int screen_h;
    PixelState** pixelState;
    Coordinate center;
    double zoom_factor;
    volatile bool auto_zoom;

    // 1 while a calculation is running, 0 when done
    volatile uint8_t calculating;
    // Incremented on every view change. Results of a calculation started with an older id are discarded.
    volatile uint32_t calculation_id;
    // Set when the screen needs to be redrawn although no calculation is running
    volatile bool needs_redraw;
    volatile uint16_t iteration_limit;
    // Incremented whenever a calculation pass finished
    volatile uint32_t passes_completed;

private:
    PixelState* row_buffer;
    PixelState* row_buffer2;
    bool* row_done;
};

#endif // FRACTALIS_STATE_H
