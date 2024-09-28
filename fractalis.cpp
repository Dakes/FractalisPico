#include "fractalis.h"
#include <cmath>

Fractalis::Fractalis(FractalisState* state) : state(state) {}

std::complex<long double> Fractalis::f_c(const std::complex<long double>& c, const std::complex<long double>& z) {
    return z * z + c;
}

std::complex<long double> Fractalis::pixel_to_point(int x, int y) {
    long double x_percent = static_cast<long double>(x) / state->screen_w;
    long double y_percent = static_cast<long double>(y) / state->screen_h;
    
    long double x_range = 3.0L / state->zoom_factor;
    long double y_range = 2.0L / state->zoom_factor;
    
    long double re = state->center.real + (x_percent - 0.5L) * x_range + state->pan_real;
    long double im = state->center.imag + (y_percent - 0.5L) * y_range + state->pan_imag;
    
    return std::complex<long double>(re, im);
}

void Fractalis::calculate_pixel(int x, int y) {
    std::complex<long double> c = pixel_to_point(x, y);
    std::complex<long double> z = 0;
    int iteration = 0;
    
    while (std::abs(z) <= 2 && iteration < MAX_ITER) {
        z = f_c(c, z);
        iteration++;
    }
    
    state->pixelState[y][x].iteration = iteration;
    state->pixelState[y][x].isComplete = true;
}

void Fractalis::zoom(long double factor) {
    state->zoom_factor *= factor;
}

void Fractalis::pan(long double dx, long double dy) {
    state->pan_real += dx / state->zoom_factor;
    state->pan_imag += dy / state->zoom_factor;
}