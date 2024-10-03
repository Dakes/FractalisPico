#ifndef FRACTALIS_H
#define FRACTALIS_H

#include "FractalisState.h"
#include "doubledouble.h"
#include <complex>

using namespace doubledouble;

class Fractalis {
public:
    Fractalis(FractalisState* state);
    void calculate_pixel(int x, int y, int iter_limit);
    void zoom(DoubleDouble factor);
    void pan(DoubleDouble dx, DoubleDouble dy);

private:
    FractalisState* state;
    std::complex<DoubleDouble> f_c(const std::complex<DoubleDouble>& c, const std::complex<DoubleDouble>& z = std::complex<DoubleDouble>(0, 0));
    std::complex<double> pixel_to_point_double(int x, int y);
    std::complex<DoubleDouble> pixel_to_point_dd(int x, int y);
    bool needsHighPrecision();
    void calculate_pixel_double(int x, int y, int iter_limit);
    void calculate_pixel_dd(int x, int y, int iter_limit);
    bool is_in_main_bulb(const std::complex<double>& c);
    bool approximately_equal(const std::complex<double>& a, const std::complex<double>& b, double epsilon = 1e-12);
};

#endif // FRACTALIS_H