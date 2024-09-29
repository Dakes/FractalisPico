#ifndef FRACTALIS_H
#define FRACTALIS_H

#include "FractalisState.h"
#include <complex>

class Fractalis {
public:
    Fractalis(FractalisState* state);
    void calculate_pixel(int x, int y, int iter_limit);
    void zoom(long double factor);
    void pan(long double dx, long double dy);

private:
    FractalisState* state;
    std::complex<long double> f_c(const std::complex<long double>& c, const std::complex<long double>& z = 0);
    std::complex<long double> pixel_to_point(int x, int y);
};

#endif // FRACTALIS_H