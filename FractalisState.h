#ifndef FRACTALIS_STATE_H
#define FRACTALIS_STATE_H

#include <cstdint>

#define MAX_ITER 255

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
    // long double z_real;         // Current z.real in the iteration
    // long double z_imag;         // Current z.imag in the iteration
    bool isComplete;       // Flag indicating if the pixel computation is complete
};

// Main state structure for the fractal zoomer
struct FractalisState {
    uint16_t screen_w;
    uint16_t screen_h;

    // 2D array of per-pixel states; accessed as pixelState[y][x]
    PixelState** pixelState;

    Coordinate center;       // Center coordinate of the current view
    double zoom_factor;      // Zoom level; higher values mean deeper zoom

    // Variables to handle panning
    long double pan_real;    // Pan offset in the real axis
    long double pan_imag;    // Pan offset in the imaginary axis

    // tracking, if the screen is rendering and if a new rendering is needed
    uint8_t rendering;
};

#endif // FRACTALIS_STATE_H