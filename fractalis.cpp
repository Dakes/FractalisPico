#include "fractalis.h"
#include "globals.h"
#include <cmath>

Fractalis::Fractalis(FractalisState* state) : state(state) {}

std::complex<DoubleDouble> Fractalis::f_c(const std::complex<DoubleDouble>& c, const std::complex<DoubleDouble>& z) {
    return z * z + c;
}

std::complex<double> Fractalis::pixel_to_point_double(int x, int y) {
    double x_percent = static_cast<double>(x) / state->screen_w;
    double y_percent = static_cast<double>(y) / state->screen_h;
    
    double x_range = 3.0 / state->zoom_factor;
    double y_range = 2.0 / state->zoom_factor;
    
    double re = state->center.real.upper + (x_percent - 0.5) * x_range + state->pan_real.upper;
    double im = state->center.imag.upper + (y_percent - 0.5) * y_range + state->pan_imag.upper;
    
    return std::complex<double>(re, im);
}


std::complex<DoubleDouble> Fractalis::pixel_to_point_dd(int x, int y) {
    DoubleDouble x_percent = DoubleDouble(x) / DoubleDouble(state->screen_w);
    DoubleDouble y_percent = DoubleDouble(y) / DoubleDouble(state->screen_h);
    
    DoubleDouble x_range = DoubleDouble(3.0) / DoubleDouble(state->zoom_factor);
    DoubleDouble y_range = DoubleDouble(2.0) / DoubleDouble(state->zoom_factor);
    
    DoubleDouble re = state->center.real + (x_percent - DoubleDouble(0.5)) * x_range + state->pan_real;
    DoubleDouble im = state->center.imag + (y_percent - DoubleDouble(0.5)) * y_range + state->pan_imag;
    
    return std::complex<DoubleDouble>(re, im);
}

bool Fractalis::needsHighPrecision() {
    // return state->zoom_factor > DoubleDouble(1e14);
    return state->zoom_factor > 1e14;
}

bool Fractalis::approximately_equal(const std::complex<double>& a, const std::complex<double>& b, double epsilon) {
    return std::abs(a - b) < epsilon;
}

// Double precision implementation
void Fractalis::calculate_pixel_double(int x, int y, int iter_limit) {
    if (x < 0 || x >= state->screen_w || y < 0 || y >= state->screen_h) {
        return;
    }
    if (state->pixelState[y][x].isComplete) {
        return;
    }

    std::complex<double> c = pixel_to_point_double(x, y);

    bool skip_optimizations = state->zoom_factor > 1e7;

    if (!skip_optimizations && is_in_main_bulb(c)) {
        state->pixelState[y][x].iteration = iter_limit;
        state->pixelState[y][x].isComplete = true;
        return;
    }

    int iteration = 0;
    std::complex<double> z(0, 0);
    std::complex<double> z_old(0, 0);
    uint8_t period = 0;

    while (std::abs(z) <= 2.0 && iteration < iter_limit) {
        z = z * z + c;
        iteration++;

        // Periodicity checking
        if (!skip_optimizations) {
            if (approximately_equal(z, z_old)) {
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

    // Smooth coloring
    if (iteration < iter_limit) {
        double log_zn = std::log(std::abs(z)) / 2;
        double nu = std::log(log_zn / std::log(2)) / std::log(2);
        state->pixelState[y][x].smooth_iteration = iteration + 1 - nu;
    } else {
        state->pixelState[y][x].smooth_iteration = 1.0f;
    }

    state->pixelState[y][x].iteration = iteration;
    state->pixelState[y][x].isComplete = true;
}


void Fractalis::calculate_pixel_dd(int x, int y, int iter_limit) {
    if (x < 0 || x >= state->screen_w || y < 0 || y >= state->screen_h) {
        return;
    }
    if (state->pixelState[y][x].isComplete) {
        return;
    }

    std::complex<DoubleDouble> c = pixel_to_point_dd(x, y);

    int iteration = 0;
    std::complex<DoubleDouble> z(0, 0);

    while ((z.real() * z.real() + z.imag() * z.imag()).sqrt() <= 2 && iteration < iter_limit) {
        z = f_c(c, z);
        iteration++;
    }

    // Smooth coloring
    if (iteration < iter_limit) {
        DoubleDouble log_zn = (z.real() * z.real() + z.imag() * z.imag()).log() / DoubleDouble(2);
        DoubleDouble log_N = DoubleDouble(16) * dd_ln2;
        DoubleDouble nu = (log_zn / log_N).log() / dd_ln2;
        state->pixelState[y][x].smooth_iteration = (iteration + DoubleDouble(1) - nu).upper;
    } else {
        state->pixelState[y][x].smooth_iteration = 1.0f;
    }

    state->pixelState[y][x].iteration = iteration;
    state->pixelState[y][x].isComplete = true;
}

void Fractalis::calculate_pixel(int x, int y, int iter_limit) {
    if (needsHighPrecision()) {
        calculate_pixel_dd(x, y, iter_limit);
    } else {
        calculate_pixel_double(x, y, iter_limit);
    }
}

void Fractalis::zoom(double factor) {
    state->zoom_factor *= 1.L + factor;
    state->calculating = 2;
    state->calculation_id++;
    state->last_updated_radius = 0;
    state->rendering = 3;
    printf("Zooming. New Zoom Factor: %f\n", state->zoom_factor);
}

void Fractalis::pan(double dx, double dy) {
    printf("Panning. dx: %f, dy: %f\n", dx, dy);
    // Calculate pixel shifts based on the actual dx and dy
    int pixel_shift_x = static_cast<int>(std::abs(dx) * state->screen_w / 3.0);
    int pixel_shift_y = static_cast<int>(std::abs(dy) * state->screen_h / 2.0);

    // Shift pixel state
    if (dx > 0) {
        state->last_pan_direction = PAN_RIGHT;
        state->shiftPixelState(-pixel_shift_x, 0);
    } else if (dx < 0) {
        state->last_pan_direction = PAN_LEFT;
        state->shiftPixelState(pixel_shift_x, 0);
    }
    if (dy > 0) {
        state->last_pan_direction = PAN_UP;
        state->shiftPixelState(0, -pixel_shift_y);
    } else if (dy < 0) {
        state->last_pan_direction = PAN_DOWN;
        state->shiftPixelState(0, pixel_shift_y);
    }

    state->calculating = 1;
    state->rendering = 3;
    state->calculation_id++;
    state->last_updated_radius = 0;
    state->pan_real += DoubleDouble(dx / state->zoom_factor);
    state->pan_imag += DoubleDouble(dy / state->zoom_factor);
}

bool Fractalis::is_in_main_bulb(const std::complex<double>& c) {
    double x = c.real();
    double y = c.imag();

    // Check for main cardioid
    double q = (x - 0.25) * (x - 0.25) + y * y;
    if (q * (q + (x - 0.25)) <= 0.25 * y * y) {
        return true;
    }

    // Check for period-2 bulb
    if ((x + 1) * (x + 1) + y * y <= 0.0625) {
        return true;
    }

    return false;
}