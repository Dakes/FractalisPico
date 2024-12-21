#ifndef FRACTALIS_STATE_H
#define FRACTALIS_STATE_H

#include <cstdint>
#include "doubledouble.h"

using namespace doubledouble;

enum PAN_DIRECTION {
    PAN_NONE = 0,
    PAN_UP,
    PAN_DOWN,
    PAN_LEFT,
    PAN_RIGHT
};

// Real and imaginary coordinates in the Mandelbrot fractal
struct Coordinate {
    DoubleDouble real;
    DoubleDouble imag;
};

// We'll define a scaling factor for smooth_iteration.
// For example, if we assume smooth_iteration generally is in the range of ~0 to a few thousands,
// using a scale factor like 256 should provide enough precision (1/256 increments).
#define SMOOTH_ITER_SCALE 1024.0f

// Per-pixel data structure to track iteration counts and current z values
struct PixelState {
    /**
     * The iteration field is used as follows:
     * - The lower bit (bit 0) is used to store the "isComplete" flag.
     * - The remaining 15 bits store the iteration count.
     *
     * Layout:
     *  bits: [15 ..................... 1 | 0 ]
     *        [      iteration_count      |complete]
     */
    uint16_t iteration;
    uint16_t smooth_iteration; // Smooth iteration count for coloring

    uint16_t getIterationCount() const {
        return iteration >> 1;
    }

    void setIterationCount(uint16_t iteration_count) {
        bool complete = isComplete();
        iteration = static_cast<uint16_t>((iteration_count << 1) | (complete ? 1 : 0));
    }

    bool isComplete() const {
        return (iteration & 0x1) != 0;
    }

    void setIsComplete(bool complete) {
        uint16_t iteration_count = getIterationCount();
        iteration = static_cast<uint16_t>((iteration_count << 1) | (complete ? 1 : 0));
    }

    void setIterationAndComplete(uint16_t iteration_count, bool complete) {
        iteration = static_cast<uint16_t>((iteration_count << 1) | (complete ? 1 : 0));
    }

    void setSmoothIterationFloat(float smooth) {
        // Clamp smooth iteration to fit into uint16_t if necessary
        // For safety, clamp to max representable value
        float scaled = smooth * SMOOTH_ITER_SCALE;
        if (scaled < 0.0f) scaled = 0.0f;
        if (scaled > 65535.0f) scaled = 65535.0f;
        smooth_iteration = static_cast<uint16_t>(scaled);
    }

    float getSmoothIterationFloat() const {
        return static_cast<float>(smooth_iteration) / SMOOTH_ITER_SCALE;
    }
};

class FractalisState {
public:

    FractalisState(int width, int height);
    ~FractalisState();

    void resetPixelComplete(int x1, int y1, int x2, int y2);
    void resetPixelComplete();
    void shiftPixelState(int dx, int dy);

    // Public members
    int screen_w;
    int screen_h;
    double ASPECT_RATIO;
    PixelState** pixelState;
    Coordinate center;
    double zoom_factor;
    DoubleDouble pan_real;
    DoubleDouble pan_imag;
    volatile int last_updated_radius;
    PAN_DIRECTION last_pan_direction;
    bool auto_zoom;

    volatile uint8_t led_skip_counter;

    // will be set to true for deep zoom factors to disable low iteration counts
    volatile bool skip_pre_render;
    volatile bool hide_ui;

   /** tracking what calculation is currently in progress. Different values have different meanings
    * 1: calculation with higher iteration limit
    * 2: calculation of pre-render in progress
    * 3: queued calculation
    */
    uint8_t calculating;
    volatile uint8_t calculation_id;

    /**
     * tracking, if the screen is rendering and if a new rendering is needed. Different values have different meanings
     * 0: Done
     * 1: Antialiasing pass
     * 2: screen rendering needed
     * 3: Whole screen render needed, in case of pan
     */
    volatile uint8_t rendering;
    volatile uint16_t iteration_limit;
    volatile uint16_t color_iteration_limit;

private:
    void resetPixelCompleteInternal(int x1, int y1, int x2, int y2);
};

#endif // FRACTALIS_STATE_H