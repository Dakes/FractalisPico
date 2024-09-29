#include "FractalisState.h"
#include <algorithm>

extern const int INITIAL_ITER;

FractalisState::FractalisState(int width, int height)
    : screen_w(width), screen_h(height), zoom_factor(1.0), pan_real(0), pan_imag(0),
      last_updated_radius(0), calculating(0), rendering(0), iteration_limit(INITIAL_ITER), color_iteration_limit(INITIAL_ITER) {
    
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