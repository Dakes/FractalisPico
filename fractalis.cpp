#include "fractalis.h"
#include <cmath>

extern const int MAX_ITER;

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

bool Fractalis::approximately_equal(const std::complex<long double>& a, const std::complex<long double>& b, long double epsilon) {
    return std::abs(a - b) < epsilon;
}

void Fractalis::calculate_pixel(int x, int y, int iter_limit) {
    // For periodicity checking. Static, to save on memory allocations
    static std::complex<long double> z = 0;
    static std::complex<long double> z_old = 0;
    static int period = 0;
    period = 0;
    z = 0;
    z_old = 0;

    if (x < 0 || x >= state->screen_w || y < 0 || y >= state->screen_h) {
        return;
    }
    if (state->pixelState[y][x].isComplete) {
        return;
    }

    std::complex<long double> c = pixel_to_point(x, y);

    bool skip_optimizations = state->zoom_factor > 1e4;

    if (!skip_optimizations && is_in_main_bulb(c)) {
        state->pixelState[y][x].iteration = iter_limit;
        state->pixelState[y][x].isComplete = true;
        return;
    }

    int iteration = 0;

    while (std::abs(z) <= 2 && iteration < iter_limit) {
        z = f_c(c, z);
        iteration++;

        // Periodicity checking
        if (!skip_optimizations) {
            if (approximately_equal(z, z_old)) {
                printf("Periodicity detected at (%d, %d)\n", x, y);
                iteration = iter_limit;
                break;
            }

            period++;
            if (period > 20) {
                period = 0;
                z_old = z;
            }
        }
    }

    state->pixelState[y][x].iteration = iteration;
    state->pixelState[y][x].isComplete = true;
}

void Fractalis::zoom(long double factor) {
    state->zoom_factor *= 1.f + factor;
}

void Fractalis::pan(long double dx, long double dy) {
    state->pan_real += dx / state->zoom_factor;
    state->pan_imag += dy / state->zoom_factor;
}

bool Fractalis::is_in_main_bulb(const std::complex<long double>& c) {
    long double x = c.real();
    long double y = c.imag();

    // Check for main cardioid
    long double q = (x - 0.25L) * (x - 0.25L) + y * y;
    if (q * (q + (x - 0.25L)) <= 0.25L * y * y) {
        return true;
    }

    // Check for period-2 bulb
    if ((x + 1.0L) * (x + 1.0L) + y * y <= 0.0625L) {
        return true;
    }

    return false;
}