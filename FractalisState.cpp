#include "FractalisState.h"
#include "globals.h"
#include <algorithm>

FractalisState::FractalisState(int width, int height)
    : screen_w(width), screen_h(height), zoom_factor(1.0), pan_real(0), pan_imag(0), led_skip_counter(0), skip_pre_render(false), hide_ui(false), last_pan_direction(PAN_NONE),
      last_updated_radius(0), calculating(0), calculation_id(0), rendering(0), iteration_limit(25), color_iteration_limit(25) {
    
    center = {-0.5, 0};
    
    pixelState = new PixelState*[screen_h];
    for (int i = 0; i < screen_h; ++i) {
        pixelState[i] = new PixelState[screen_w];
        for (int j = 0; j < screen_w; ++j) {
            pixelState[i][j] = {0, false};
        }
    }
}

FractalisState::~FractalisState() {
    for (int i = 0; i < screen_h; ++i) {
        delete[] pixelState[i];
    }
    delete[] pixelState;
}

void FractalisState::resetPixelComplete(int x1, int y1, int x2, int y2) {
    resetPixelCompleteInternal(x1, y1, x2, y2);
}

void FractalisState::resetPixelComplete() {
    resetPixelCompleteInternal(0, 0, screen_w - 1, screen_h - 1);
}

void FractalisState::resetPixelCompleteInternal(int x1, int y1, int x2, int y2) {
    // Ensure coordinates are within bounds
    x1 = std::max(0, std::min(x1, screen_w - 1));
    y1 = std::max(0, std::min(y1, screen_h - 1));
    x2 = std::max(0, std::min(x2, screen_w - 1));
    y2 = std::max(0, std::min(y2, screen_h - 1));

    // Ensure x1 <= x2 and y1 <= y2
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);

    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            pixelState[y][x].isComplete = false;
        }
    }
}

void FractalisState::shiftPixelState(int dx, int dy) {
    // Shift the pixel state in-place without creating a new array
    if (dx == 0 && dy == 0) {
        return;
    }

    // Shifting pixels horizontally (x-axis)
    if (dx != 0) {
        for (int y = 0; y < screen_h; ++y) {
            if (dx > 0) {
                // Shift to the right
                for (int x = screen_w - 1; x >= dx; --x) {
                    pixelState[y][x] = pixelState[y][x - dx];
                }
                // Mark newly exposed pixels on the left as incomplete
                for (int x = 0; x < dx; ++x) {
                    pixelState[y][x].isComplete = false;
                    pixelState[y][x].iteration = 0;
                }
            } else {
                // Shift to the left
                for (int x = 0; x < screen_w + dx; ++x) {
                    pixelState[y][x] = pixelState[y][x - dx];
                }
                // Mark newly exposed pixels on the right as incomplete
                for (int x = screen_w + dx; x < screen_w; ++x) {
                    pixelState[y][x].isComplete = false;
                    pixelState[y][x].iteration = 0;
                }
            }
        }
    }

    // Shifting pixels vertically (y-axis)
    if (dy != 0) {
        if (dy > 0) {
            // Shift downwards
            for (int y = screen_h - 1; y >= dy; --y) {
                for (int x = 0; x < screen_w; ++x) {
                    pixelState[y][x] = pixelState[y - dy][x];
                }
            }
            // Mark newly exposed pixels at the top as incomplete
            for (int y = 0; y < dy; ++y) {
                for (int x = 0; x < screen_w; ++x) {
                    pixelState[y][x].isComplete = false;
                    pixelState[y][x].iteration = 0;
                }
            }
        } else {
            // Shift upwards
            for (int y = 0; y < screen_h + dy; ++y) {
                for (int x = 0; x < screen_w; ++x) {
                    pixelState[y][x] = pixelState[y - dy][x];
                }
            }
            // Mark newly exposed pixels at the bottom as incomplete
            for (int y = screen_h + dy; y < screen_h; ++y) {
                for (int x = 0; x < screen_w; ++x) {
                    pixelState[y][x].isComplete = false;
                    pixelState[y][x].iteration = 0;
                }
            }
        }
    }
}