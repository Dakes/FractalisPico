#ifndef PALETTE_H
#define PALETTE_H

#include <cstdint>
#include "FractalisState.h"

/**
 * Coloring is split in two steps:
 * - position(): smooth iteration count -> palette position. Done once when a pixel is calculated.
 * - Palette::render(): palette position -> RGB565 via lookup table, plus relief shading from the neighboring pixels.
 *   Cheap enough to redraw the whole screen every frame, so palettes can be switched and animated without
 *   recalculating anything.
 */
namespace palette {

// Fixed point palette position: t = log(1 + smooth iteration) / 2, stored as t * 2^POSITION_BITS (24 bit).
// The upper 16 bit go to PixelState::color, the lower 8 bit to PixelState::fine.
constexpr int POSITION_BITS = 20;
constexpr int LUT_BITS = 10;
constexpr int LUT_SIZE = 1 << LUT_BITS;

uint32_t position(float smooth_iteration);

struct ColorStop {
    float position;  // 0-1
    uint8_t r, g, b;
};

struct Definition {
    const char* name;
    const ColorStop* stops;
    int stop_count;
    float cycles;  // palette repetitions per unit of t, higher = more color bands
    float offset;  // shifts the palette
};

class Palette {
public:
    Palette();

    void select(int index);
    void next();
    const char* name() const;

    // Shifts the colors along the palette, for color cycling animations. 1.0 = one full palette cycle.
    void set_phase(float phase);

    /**
     * Auto contrast: stretches the palette over the range of values on screen. Call after a calculation pass
     * finished. The colors then smoothly move to the new mapping with every animate() call.
     */
    void analyze(PixelState* const* pixels, int width, int height);
    // Moves the color mapping towards the analyzed target. Returns true while still changing.
    bool animate();

    bool shading = true;

    /**
     * Draws the pixel state into an RGB565 frame buffer (in display byte order).
     */
    void render(PixelState* const* pixels, int width, int height, uint16_t* frame_buffer) const;
    // Only the rows first_row .. first_row + rows - 1, into a buffer of that many rows
    void render_rows(PixelState* const* pixels, int width, int height, int first_row, int rows,
                     uint16_t* out_rows) const;

private:
    int current;
    uint32_t phase_offset;  // in LUT entries << 16
    // Mapping position -> LUT index: (position - range_start) * cycles
    float range_start, cycles;
    float target_range_start, target_cycles;
    uint32_t lut_step;      // LUT entries per position unit, 16.16 fixed point
    void update_lut_step();
    uint16_t lut[LUT_SIZE]; // RGB565, native byte order
};

}  // namespace palette

#endif // PALETTE_H
