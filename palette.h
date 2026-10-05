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
constexpr int MAX_WIDTH = 320;  // of the screen

uint32_t position(float smooth_iteration);
// Orbit traps: palette position from the smallest squared distance of the orbit to the trap (log scale)
uint32_t trap_position(float distance_sq);
// Period map: a color per period (golden ratio steps along the palette), slightly shaded by how close the orbit came
// to 0, so the relief light shows the shape of each domain
uint32_t period_position(int period, float distance_sq);
// Stripe average coloring: sum and packed count / last term from the iteration (see record_trap() in fractalis.cpp),
// blend 0-1 between the average without and with the last term
uint32_t stripe_position(float sum, int packed, float blend);
/**
 * Distance estimation: from log2 of the distance to the set in pixels, 2^16 positions per octave. Closer is higher,
 * like the iteration count, so the palettes and the relief run the same way. All of them are at or above
 * DISTANCE_BASE, the iteration counts below it.
 */
constexpr uint32_t DISTANCE_BASE = 1u << 23;
constexpr uint32_t DISTANCE_OCTAVE = 1u << 16;
uint32_t distance_position(float log2_distance);

/**
 * Supersampling: the range of the sub-sample positions on a log scale in 3 bits. 1 = none, then factors of 8 up
 * to 7 = the whole palette.
 */
uint8_t spread_level(uint32_t range);
// Range of positions a level stands for. 0 for level 0 and 1.
uint32_t spread_width(uint8_t level);

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

int count();
const char* name(int index);
// The color of a palette at x (0-1 is once through it), RGB565 in native byte order. For previews.
uint16_t sample(int index, float x);
// The last palette is one color, set here (RGB565, native byte order). Palettes showing it have to select() it again.
constexpr int SOLID = 15;
void set_solid_color(uint16_t color);
// h 0-1 around the color wheel, s and v 0-1. RGB565, native byte order.
uint16_t hsv(float h, float s, float v);

class Palette {
public:
    Palette();

    void select(int index);
    void next();
    int index() const { return current; }
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
    // Edge glow: lighter where the colors change fast from pixel to pixel, thin bright outlines along the filaments
    bool edges = false;
    /**
     * Outlines of distance estimation: escaped pixels with a distance position (see Fractalis::distance_colors())
     * get darker towards the set. 0 = none, then thin to bold.
     */
    static constexpr int OUTLINE_COUNT = 4;
    void set_outlines(int option);
    /**
     * Direction the relief light comes from, in radians: 0 = from the right, pi / 2 = from the bottom (screen
     * coordinates). Starts at the top left.
     */
    void set_light(float angle);
    // How many times the palette repeats over the range on screen, relative to the auto contrast (1 = normal)
    void set_bands(float bands);
    float get_bands() const { return bands; }

    /**
     * Draws the pixel state into an RGB565 frame buffer (in display byte order).
     */
    void render(PixelState* const* pixels, int width, int height, uint16_t* frame_buffer) const;
    // Only the rows first_row .. first_row + rows - 1, into a buffer of that many rows
    // content: see FractalisState::content, pixels outside of it that aren't calculated yet show its edge
    void render_rows(PixelState* const* pixels, int width, int height, int first_row, int rows,
                     uint16_t* out_rows, const ScreenRect* content = nullptr) const;
    // The same with its own scratch space (2 * width), so it can run alongside the drawing of the frames
    void render_rows(PixelState* const* pixels, int width, int height, int first_row, int rows,
                     uint16_t* out_rows, const ScreenRect* content, PixelState* scratch) const;

private:
    int current;
    uint32_t phase_offset;  // in LUT entries << 16
    // Mapping position -> LUT index: (position - range_start) * cycles
    float range_start, cycles;
    float target_range_start, target_cycles;
    uint32_t lut_step;      // LUT entries per position unit, 16.16 fixed point
    float bands = 1.0f;
    float light_x = -0.70710678f, light_y = -0.70710678f;  // the top left
    void update_lut_step();
    uint16_t lut[LUT_SIZE]; // RGB565, native byte order
    // Running sums of the 5/6/5 bit channels of lut: prefix[c][i] = sum of lut[0 .. i-1]. The average color of
    // any range of the palette costs a subtraction and a division.
    uint16_t prefix[3][LUT_SIZE + 1];
    uint16_t average(uint32_t start, uint32_t width) const;
    // Brightness 0-255 (255 = unchanged) over the distances OUTLINE_NEAR .. OUTLINE_FAR octaves (log2 pixels)
    static constexpr int OUTLINE_STEPS = 256;
    int outline_option = 0;
    uint8_t outline[OUTLINE_STEPS];
};

}  // namespace palette

#endif // PALETTE_H
