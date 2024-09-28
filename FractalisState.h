#ifndef FRACTALIS_STATE_H
#define FRACTALIS_STATE_H

#include <cstdint>


enum PAN_DIRECTION {
    PAN_NONE = 0,
    PAN_UP,
    PAN_DOWN,
    PAN_LEFT,
    PAN_RIGHT
};

// Real and imaginary coordinates in the Mandelbrot fractal
struct Coordinate {
    long double real;
    long double imag;
};

// Per-pixel data structure to track iteration counts and current z values
struct PixelState {
    uint8_t iteration;    // Current iteration count
    bool isComplete;       // Flag indicating if the pixel computation is complete
};

// Main state structure for the fractal zoomer
struct FractalisState {
    // will never change during runtime
    int screen_w;
    int screen_h;

    // 2D array of per-pixel states; accessed as pixelState[y][x]
    PixelState** pixelState;

    Coordinate center;       // Center coordinate of the current view
    double zoom_factor;      // Zoom level; higher values mean deeper zoom

    // Variables to handle panning
    long double pan_real;    // Pan offset in the real axis
    long double pan_imag;    // Pan offset in the imaginary axis

    int last_updated_radius;

   /** tracking what calculation is currently in progress. Different values have different meanings
    * 1: calculation with higher iteration limit
    * 2: calculation in progress
    * 3: queued calculation
    */
    uint8_t calculating;

    /**
     * tracking, if the screen is rendering and if a new rendering is needed. Different values have different meanings
     * 1: Antialiasing pass
     * 2: screen rendering needed
     */
    uint8_t rendering;

    // dymamic iteration limit. Should be low initially and for a second render increase
    uint8_t iteration_limit;

};

#endif // FRACTALIS_STATE_H