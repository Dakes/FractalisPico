#ifndef FRACTALIS_STATE_H
#define FRACTALIS_STATE_H

#include <cstdint>
#include "fixed.h"

// Real and imaginary coordinates in the Mandelbrot fractal
struct Coordinate {
    Fixed real;
    Fixed imag;
};

/**
 * Per-pixel result of the calculation, 4 bytes.
 * flags: bit 0 COMPLETE, bits 1-2 kind (IN_SET, VALID), bits 3-5 supersampling spread, bits 6-7 coverage.
 * The kind bits encode 4 states:
 *   neither          empty, nothing to display
 *   VALID            escaped, position is the (final or preview) palette position
 *   VALID + IN_SET   in the set (black)
 *   IN_SET only      in the set, but shown in color: the preview color while undecided at the current iteration
 *                    limit, or the orbit trap color
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
    bool isInSetColored() const { return (flags & KIND_MASK) == IN_SET; }

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

    /**
     * Black pixels don't need a position, their fine byte marks why they are black: proven to be in the set (not
     * calculated again), or undecided at the last limit with its z stored in slot `color` (continues there).
     */
    static constexpr uint8_t PROVEN = 1, RESUMABLE = 2;
    bool isBlack() const { return (flags & KIND_MASK) == (VALID | IN_SET); }
    bool isProven() const { return isBlack() && fine == PROVEN; }
    bool isResumable() const { return isBlack() && fine == RESUMABLE; }
    void setInSetProven() { setInSet(); fine = PROVEN; }
    void setInSetResumable(uint16_t slot) { setInSet(); color = slot; fine = RESUMABLE; }
    void clearMark() { if (isBlack()) { color = 0; fine = 0; } }
    // In the set at the current limit, the preview color stays until the final pass
    void setInSetKeepingPreview() { flags = COMPLETE | IN_SET; }
    void dropPreviewColor() { if (isInSetColored()) flags |= VALID; }
    // In the set, colored by the orbit trap
    void setInSetColored(uint32_t position) { flags = COMPLETE | IN_SET; fine = 0; setPosition(position); }

    // Keep the displayed value, but mark the pixel for recalculation
    void markIncomplete() { flags &= ~COMPLETE; }
    void clear() { color = 0; flags = 0; fine = 0; }
};

// Area of the screen [x0, x1) x [y0, y1)
struct ScreenRect {
    int x0, y0, x1, y1;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};

/**
 * The pixels of a pass are calculated on grids around the screen center, coarse to fine (see
 * Fractalis::next_in_order()). Pixels without a value yet show the nearest calculated point of the finest grid that
 * has one. colored: only points that show a color count. nullptr if there is none.
 */
inline const PixelState* nearest_calculated(PixelState* const* pixels, int width, int height, int x, int y,
                                            bool colored) {
    const int cx = width / 2, cy = height / 2;
    for (int step = 2; step <= 64; step *= 2) {
        // Nearest grid point, also for negative offsets
        int gx = cx + ((x - cx + step / 2 + 64 * step) / step - 64) * step;
        int gy = cy + ((y - cy + step / 2 + 64 * step) / step - 64) * step;
        // At the edges the nearest one can be off the screen, then the one on this side
        if (gx >= width) gx -= step;
        if (gy >= height) gy -= step;
        if (gx < 0) gx += step;
        if (gy < 0) gy += step;
        if (gx < 0 || gx >= width || gy < 0 || gy >= height) continue;
        const PixelState& p = pixels[gy][gx];
        if (colored ? p.showsColor() : p.isValid()) return &p;
    }
    return nullptr;
}

/**
 * Smoother than nearest_calculated(): the palette position blended between the 4 calculated points around the
 * pixel, on the finest grid where all of them are done. Points in the set count as black: if they weigh more than
 * the colored ones, the pixel is black.
 * calculated: only points calculated in this pass count as done (not the preview of a zoom), step is set to the
 * grid used.
 */
enum class Blend { NONE, IN_SET, COLOR };  // no grid done around it yet / all 4 points in the set / position set
inline Blend interpolated_position(PixelState* const* pixels, int width, int height, int x, int y,
                                  uint32_t& position, bool calculated = false, int* used_step = nullptr) {
    const int cx = width / 2, cy = height / 2;
    for (int step = 2; step <= 64; step *= 2) {
        // The grid cell around the pixel
        int x0 = cx + ((x - cx + 64 * step) / step - 64) * step;
        int y0 = cy + ((y - cy + 64 * step) / step - 64) * step;
        if (x0 == x && y0 == y) continue;  // a point of this grid itself
        const int xs[2] = {x0, x0 + step}, ys[2] = {y0, y0 + step};
        const float fx = static_cast<float>(x - x0) / step, fy = static_cast<float>(y - y0) / step;
        float sum = 0, weight_sum = 0, black = 0;
        bool complete = true;
        for (int j = 0; j < 2 && complete; ++j) {
            for (int i = 0; i < 2; ++i) {
                // Corners off the screen don't exist, the others must be calculated
                if (xs[i] < 0 || xs[i] >= width || ys[j] < 0 || ys[j] >= height) continue;
                const PixelState& p = pixels[ys[j]][xs[i]];
                if (calculated ? !p.isComplete() : !p.isValid()) {
                    complete = false;
                    break;
                }
                float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                if (!p.showsColor()) {
                    black += w;
                    continue;
                }
                sum += w * static_cast<float>(p.position());
                weight_sum += w;
            }
        }
        if (!complete) continue;
        if (used_step) *used_step = step;
        if (weight_sum <= black) return Blend::IN_SET;
        position = static_cast<uint32_t>(sum / weight_sum);
        return Blend::COLOR;
    }
    return Blend::NONE;
}

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
    // The part of the screen that still shows the content from before the last pan or zoom (outside it, the
    // pixels were cleared)
    ScreenRect content;
    // Bounding box of the pixels that have something to show
    ScreenRect valid_bounds() const;
    /**
     * Pixels that aren't calculated yet are drawn blended from the calculated grid points around them. Before
     * zooming in, that is stored in them as a preview: after the zoom the grid points are somewhere else, the
     * pixels in between would show as dots and blocks.
     */
    void store_blends();
    /**
     * The pixels that aren't calculated yet show a zoom preview (scalePixelState()): the shading takes the slope
     * between pixels of the same kind, see Palette::render_rows(). Cleared when the preview is the image of the
     * same view (see Fractalis::recalculate_all()).
     */
    volatile bool zoom_preview = false;
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
