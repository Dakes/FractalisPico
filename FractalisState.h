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

/**
 * Per-pixel result of the calculation, 4 bytes.
 * flags: bit 0 COMPLETE, bits 1-2 kind (IN_SET, VALID), bits 3-5 supersampling spread, bits 6-7 coverage.
 * The kind bits encode 4 states:
 *   neither          empty, nothing to display
 *   VALID            escaped, position is the (final or preview) palette position
 *   VALID + IN_SET   in the set (black)
 *   IN_SET only      in the set at the current iteration limit, but still shows the color of the preview
 */
struct PixelState {
    enum Flags : uint8_t {
        COMPLETE = 1 << 0,  // calculated with the current view and iteration limit
        IN_SET   = 1 << 1,  // did not escape within the iteration limit
        VALID    = 1 << 2,
    };
    static constexpr int SPREAD_SHIFT = 3;
    static constexpr int COVERAGE_SHIFT = 6;
    static constexpr uint8_t KIND_MASK = IN_SET | VALID;

    // palette position, upper 16 and lower 8 bit, see palette.h
    uint16_t color;
    uint8_t flags;
    uint8_t fine;

    bool isComplete() const { return flags & COMPLETE; }
    bool isInSet() const { return flags & IN_SET; }
    // Has something to display (also previews after zoom/pre-render)
    bool isValid() const { return flags & KIND_MASK; }
    // Escaped, with a final or preview value
    bool hasPosition() const { return (flags & KIND_MASK) == VALID; }
    // Drawn in color (not black)
    bool showsColor() const { return (flags & KIND_MASK) == VALID || (flags & KIND_MASK) == IN_SET; }
    bool showsPreviewColor() const { return (flags & KIND_MASK) == IN_SET; }

    uint32_t position() const { return (static_cast<uint32_t>(color) << 8) | fine; }
    void setPosition(uint32_t position) {
        color = static_cast<uint16_t>(position >> 8);
        fine = static_cast<uint8_t>(position & 0xFF);
    }

    /**
     * Supersampling: the position is the mean of the sub-samples that escaped. spread is the log scale range of
     * their positions (1 = none, see palette::spread_width), coverage the share of sub-samples in the set in
     * quarters (0-3). 0 = not supersampled.
     */
    uint8_t spread() const { return (flags >> SPREAD_SHIFT) & 7; }
    uint8_t coverage() const { return flags >> COVERAGE_SHIFT; }
    bool isSupersampled() const { return spread() != 0; }
    void setSupersampled(uint8_t spread, uint8_t coverage) {
        flags = static_cast<uint8_t>((flags & (COMPLETE | KIND_MASK)) | (spread << SPREAD_SHIFT)
                                     | (coverage << COVERAGE_SHIFT));
    }

    void setEscaped(uint32_t position) { flags = COMPLETE | VALID; fine = 0; setPosition(position); }
    void setInSet() { color = 0; fine = 0; flags = COMPLETE | VALID | IN_SET; }
    // In the set at the current limit, the preview color stays until the final pass
    void setInSetKeepingPreview() { flags = COMPLETE | IN_SET; }
    void dropPreviewColor() { if (showsPreviewColor()) flags |= VALID; }

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
    // Auto zoom waits for supersampling as well, otherwise the views are not supersampled during auto zoom
    volatile bool auto_zoom_full_quality;

    // 1 while a calculation is running, 0 when done
    volatile uint8_t calculating;
    // The running calculation only adds sub-samples (supersampling) to the finished view
    volatile bool supersampling;
    // Incremented on every view change. Results of a calculation started with an older id are discarded.
    volatile uint32_t calculation_id;
    // Set when the screen needs to be redrawn although no calculation is running
    volatile bool needs_redraw;
    volatile uint16_t iteration_limit;
    // Incremented whenever a calculation pass finished
    volatile uint32_t passes_completed;
    // Iteration limit of the last pass that covered the whole screen for the current view. 0 = none yet
    volatile uint16_t completed_limit;

private:
    PixelState* row_buffer;
    PixelState* row_buffer2;
    bool* row_done;
};

#endif // FRACTALIS_STATE_H
