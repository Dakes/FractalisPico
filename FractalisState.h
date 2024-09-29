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


class FractalisState {
public:
    FractalisState(int width, int height);
    ~FractalisState();

    void resetPixelComplete(int x1, int y1, int x2, int y2);
    void resetPixelComplete();

    // Public members
    int screen_w;
    int screen_h;
    PixelState** pixelState;
    Coordinate center;
    double zoom_factor;
    long double pan_real;
    long double pan_imag;
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
    uint8_t color_iteration_limit;


private:
    void resetPixelCompleteInternal(int x1, int y1, int x2, int y2);
};

#endif // FRACTALIS_STATE_H