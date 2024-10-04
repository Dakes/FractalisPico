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

// Per-pixel data structure to track iteration counts and current z values
struct PixelState {
    uint16_t iteration;      // Current iteration count
    bool isComplete;        // Flag indicating if the pixel computation is complete
    float smooth_iteration; // Smooth iteration count for coloring
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