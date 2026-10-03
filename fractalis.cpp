#include "fractalis.h"
#include "globals.h"
#include "palette.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <type_traits>

namespace {

struct Escape {
    int iterations;
    float magnitude_sq;  // |z|^2 after escaping
    bool in_set;
    bool aborted;
    float trap = INFINITY;  // orbit trap: smallest squared distance of the orbit to the trap shape
    bool proven = false;    // in the set because the orbit was found to repeat, not because it reached the limit
};

/**
 * Orbit traps: squared distance of z to the trap shape. The shape is a template parameter, so the iteration loops
 * without a trap are exactly as fast as before.
 */
template <int TRAP>
inline float trap_distance(float zr, float zi, float magnitude_sq) {
    if (TRAP == Fractalis::TRAP_POINT) return magnitude_sq;            // the origin
    if (TRAP == Fractalis::TRAP_CROSS) return std::min(zr * zr, zi * zi);  // both axes ("Pickover stalks")
    float ring = magnitude_sq - 1.0f;                                   // unit circle, ~ 2 * distance
    return 0.25f * ring * ring;
}

/**
 * Which part of the orbit counts for the trap: only iterations where z changes noticeably from one pixel to the
 * next. Before that, all pixels nearby follow practically the same orbit (at deep zooms most of it), which would
 * give them all the same trap value. Tracked with the derivative dz/dc times the pixel size:
 *   d' = 2 z d + pixel size,  counts once |d| > 1e-5 |z|
 * Once it counts, it counts for the rest of the orbit.
 */
template <typename T>
struct TrapScope {
    T dr = 0, di = 0;
    bool counting = false;
    // before z = z^2 + c, with the old z
    void step(T zr, T zi, T pixel_size) {
        if (counting) return;
        T nr = 2 * (zr * dr - zi * di) + pixel_size;
        di = 2 * (zr * di + zi * dr);
        dr = nr;
    }
    // after it, with the new |z|^2
    bool counts(T magnitude_sq) {
        if (!counting) counting = dr * dr + di * di > T(1e-10) * magnitude_sq;
        return counting;
    }
};

// Calls f with the trap shape as a compile time constant
template <typename F>
Escape with_trap(int trap, const F& f) {
    switch (trap) {
        case Fractalis::TRAP_POINT: return f(std::integral_constant<int, Fractalis::TRAP_POINT>());
        case Fractalis::TRAP_CROSS: return f(std::integral_constant<int, Fractalis::TRAP_CROSS>());
        case Fractalis::TRAP_RING: return f(std::integral_constant<int, Fractalis::TRAP_RING>());
        default: return f(std::integral_constant<int, Fractalis::TRAP_OFF>());
    }
}

// Long calculations check regularly, if their result is still needed
constexpr int ABORT_CHECK_MASK = 511;

template <typename T>
bool is_in_main_bulb(T x, T y) {
    // Check for main cardioid
    T q = (x - T(0.25)) * (x - T(0.25)) + y * y;
    if (q * (q + (x - T(0.25))) <= T(0.25) * y * y) {
        return true;
    }

    // Check for period-2 bulb
    if ((x + 1) * (x + 1) + y * y <= T(0.0625)) {
        return true;
    }

    return false;
}

/**
 * z = z^2 + c with plain real arithmetic. std::complex is a lot slower: std::abs() calls hypot() and the
 * multiplication has extra NaN/Inf handling (__muldc3).
 * Periodicity checking (Brent): z is saved at exponentially growing intervals, if the orbit comes back to the
 * saved value it is caught in a cycle and will never escape.
 */
template <int TRAP, typename T, typename Abort>
Escape iterate(T cr, T ci, int iter_limit, T epsilon, bool check_periodicity, float pixel_size,
               const Abort& abort) {
    T zr = 0, zi = 0, zr2 = 0, zi2 = 0;
    T saved_r = 0, saved_i = 0;
    int save_at = 8;
    float trap = INFINITY;
    TrapScope<float> scope;

    for (int n = 0; n < iter_limit; ++n) {
        if (TRAP != Fractalis::TRAP_OFF) scope.step(static_cast<float>(zr), static_cast<float>(zi), pixel_size);
        zi = (zr + zr) * zi + ci;
        zr = zr2 - zi2 + cr;
        zr2 = zr * zr;
        zi2 = zi * zi;
        if (zr2 + zi2 > static_cast<T>(BAILOUT_SQ)) {
            return {n + 1, static_cast<float>(zr2 + zi2), false, false, trap};
        }
        if (TRAP != Fractalis::TRAP_OFF && scope.counts(static_cast<float>(zr2 + zi2))) {
            trap = std::min(trap, trap_distance<TRAP>(static_cast<float>(zr), static_cast<float>(zi),
                                                      static_cast<float>(zr2 + zi2)));
        }
        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
        if (check_periodicity) {
            if (std::abs(zr - saved_r) + std::abs(zi - saved_i) < epsilon) {
                return {iter_limit, 0.0f, true, false, trap, true};
            }
            if (n == save_at) {
                saved_r = zr;
                saved_i = zi;
                save_at *= 2;
            }
        }
    }
    return {iter_limit, 0.0f, true, false, trap};
}

// Lean double-double arithmetic for the iteration without perturbation
struct DD {
    double hi, lo;
};

inline DD quick_two_sum(double a, double b) {
    double s = a + b;
    return {s, b - (s - a)};
}

inline DD dd_add(DD a, DD b) {
    double s = a.hi + b.hi;
    double v = s - a.hi;
    double e = (a.hi - (s - v)) + (b.hi - v);
    return quick_two_sum(s, e + a.lo + b.lo);
}

inline DD dd_sub(DD a, DD b) {
    return dd_add(a, {-b.hi, -b.lo});
}

inline DD dd_mul(DD a, DD b) {
    double p = a.hi * b.hi;
    double e = std::fma(a.hi, b.hi, -p);  // hardware accelerated on the RP2350 (DCP)
    return quick_two_sum(p, e + (a.hi * b.lo + a.lo * b.hi));
}

// Far outside of the set, so the fixed point numbers don't overflow (range +-128)
Fixed clamp_coordinate(const Fixed& value) {
    constexpr double LIMIT = 16.0;
    double v = value.to_double();
    return v > LIMIT ? Fixed(LIMIT) : v < -LIMIT ? Fixed(-LIMIT) : value;
}

// z = z^2 + c in fixed point
inline void square_add(Fixed& zr, Fixed& zi, const Fixed& cr, const Fixed& ci) {
    Fixed zri = zr * zi;
    zr = zr * zr - zi * zi + cr;
    zi = zri.twice() + ci;
}

/**
 * Minibrots are (almost) exact copies of the whole set, so the main cardioid/bulb check also works for them in
 * minibrot coordinates (c - nucleus) / size. The copies are slightly distorted, so the check is done with a margin:
 * the cardioid shrunk by 10% towards the nucleus and a smaller period 2 bulb. Pixels close to the border are
 * calculated normally.
 */
bool is_safely_in_main_bulb(float x, float y) {
    if (std::abs(x) > 1.5f || std::abs(y) > 1.0f) return false;
    float sx = x * 1.1f - 0.25f, sy = y * 1.1f;
    float q = sx * sx + sy * sy;
    return q * (q + sx) <= 0.25f * sy * sy || (x + 1) * (x + 1) + y * y < 0.04f;
}

struct Minibrot {
    int period;
    bool usable;  // c is safely inside, so the check is worth it
    Fixed nr, ni;  // nucleus
    bool cardioid;
    double scale_r, scale_i;  // see is_in_component()
};

/**
 * d = c - nucleus. Cardioids: d * scale is in the coordinates of the whole set (scale = 1 / size).
 * Discs: d * scale is the multiplier of the cycle (to first order), the disc ends where it reaches 1. Only
 * 0.3 is used, that's safe for any shape between disc and cardioid.
 */
bool is_in_component(bool cardioid, float x, float y) {
    if (cardioid) return is_safely_in_main_bulb(x, y);
    return x * x + y * y < 0.09f;
}

/**
 * Finds the hyperbolic component (minibrot or bulb) that contains c, starting with a guess of its period (or a
 * multiple of it): Newton's method for z_period(nucleus) = 0, then the size estimate by Claude Heiland-Allen:
 *   size = 1 / (b * l^2),  l = prod 2 z_k,  b = sum 1 / l_k  over the nucleus orbit
 * Returns false if it didn't converge. usable is false if c isn't safely inside, then the period is still valid.
 */
template <typename Interrupt>
bool find_minibrot(Fixed cr, Fixed ci, int period, Minibrot& out, const Interrupt& interrupt) {
    const Fixed c0r = cr, c0i = ci;
    double last_step = INFINITY;
    bool converged = false;
    for (int step = 0; step < 12 && !converged; ++step) {
        if (interrupt()) return false;
        Fixed zr = 0.0, zi = 0.0;
        double zrd = 0, zid = 0;
        double dr = 0, di = 0;  // dz/dc
        for (int i = 0; i < period; ++i) {
            double ndr = 2 * (zrd * dr - zid * di) + 1;
            di = 2 * (zrd * di + zid * dr);
            dr = ndr;
            square_add(zr, zi, cr, ci);
            zrd = zr.to_double();
            zid = zi.to_double();
            if (zrd * zrd + zid * zid > 4.0) return false;
        }
        double d = dr * dr + di * di;
        if (!(d > 0) || !std::isfinite(d)) return false;
        double sr = (zrd * dr + zid * di) / d;
        double si = (zid * dr - zrd * di) / d;
        if (!(std::abs(sr) + std::abs(si) < 1.0)) return false;
        cr -= Fixed(sr);
        ci -= Fixed(si);
        // |z_period| relative to the size of the minibrot ~ 1 / |dz/dc|
        double step_size = std::sqrt((sr * sr + si * si) * d);
        converged = step_size < 1e-6;
        // Stuck at the precision limit or diverging
        if (!converged && step_size > last_step * 0.5 && step > 2) break;
        last_step = step_size;
    }
    if (!converged && last_step > 1e-3) return false;

    // The actual period divides the guess: the first z_k of the nucleus orbit that is (almost) 0
    double min_sq = INFINITY;
    int actual = period;
    {
        // first pass: the smallest |z_k|, second pass: the first k that is about as small
        for (int pass = 0; pass < 2; ++pass) {
            Fixed zr = 0.0, zi = 0.0;
            for (int k = 1; k <= period; ++k) {
                square_add(zr, zi, cr, ci);
                double zrd = zr.to_double(), zid = zi.to_double();
                double m = zrd * zrd + zid * zid;
                if (pass == 0) {
                    min_sq = std::min(min_sq, m);
                } else if (period % k == 0 && m <= min_sq * 1e6) {
                    actual = k;
                    break;
                }
            }
        }
    }

    // Size estimate and shape: a minibrot is a cardioid, but the bulbs attached to it are discs. Near the nucleus
    // the multiplier of the cycle is lambda = alpha * d + beta * d^2 (d = c - nucleus). The discriminant
    // e = beta / alpha^2 is 1/2 for a cardioid (like the main cardioid) and 0 for a disc (like the period 2 bulb).
    // With z_p = A d + B d^2 and l = prod 2 z_k (k = 1 .. period - 1), l' = dl/dz_1:
    //   e = 1/2 + (l B + l' A) / (2 l^2 A^2)
    using complex = std::complex<double>;
    complex l = 1, dl = 0, b = 1, a = 0, a2 = 0;
    Fixed zr = 0.0, zi = 0.0;
    for (int k = 0; k < actual; ++k) {
        complex z(zr.to_double(), zi.to_double());
        a2 = 2.0 * (a * a + z * a2);
        a = 2.0 * z * a + 1.0;
        if (k >= 1) {
            dl = 2.0 * (l * l + z * dl);
            l = 2.0 * z * l;
            b += 1.0 / l;
        }
        square_add(zr, zi, cr, ci);
    }
    complex e = 0.5 + (l * (a2 * 0.5) + dl * a) / (2.0 * l * l * a * a);
    out.cardioid = std::abs(e - 0.5) < 0.1;
    complex scale = out.cardioid ? b * l * l : 2.0 * l * a;  // 1 / size, or alpha
    if (!std::isfinite(scale.real()) || !std::isfinite(scale.imag()) || !std::isfinite(std::abs(e))) {
        return false;
    }
    out.scale_r = scale.real();
    out.scale_i = scale.imag();
    out.period = actual;
    out.nr = cr;
    out.ni = ci;

    // Only useful if the reference itself is inside
    double dcr = (c0r - cr).to_double(), dci = (c0i - ci).to_double();
    float x = static_cast<float>(dcr * out.scale_r - dci * out.scale_i);
    float y = static_cast<float>(dcr * out.scale_i + dci * out.scale_r);
    out.usable = is_in_component(out.cardioid, x, y);
    return true;
}

/**
 * Interior distance estimate: if c has an attracting cycle, every point closer than b / 4 to c is in the set as
 * well (Koebe 1/4 theorem). With F = f^period at a point z of the cycle:
 *   b = (1 - |F_z|^2) / |F_zc + F_zz F_c / (1 - F_z)|
 * z starts at a point of the orbit close to the cycle and is refined with Newton's method for F(z) = z.
 * Returns the radius (with a margin), 0 if there's no attracting cycle.
 */
template <typename Interrupt>
double interior_radius(Fixed zr, Fixed zi, const Fixed& cr, const Fixed& ci, int period,
                       const Interrupt& interrupt) {
    using complex = std::complex<double>;
    double last_step = INFINITY;
    for (int step = 0; step < 10; ++step) {
        if (interrupt()) return 0;
        Fixed wr = zr, wi = zi;
        complex w(wr.to_double(), wi.to_double());
        complex fz = 1, fc = 0, fzz = 0, fzc = 0;
        for (int i = 0; i < period; ++i) {
            fzz = 2.0 * (fz * fz + w * fzz);
            fzc = 2.0 * (fz * fc + w * fzc);
            fc = 2.0 * w * fc + 1.0;
            fz = 2.0 * w * fz;
            square_add(wr, wi, cr, ci);
            w = complex(wr.to_double(), wi.to_double());
            if (std::norm(w) > 4.0) return 0;
        }
        if (!(std::abs(fz) < 1.0)) return 0;
        complex g = complex((wr - zr).to_double(), (wi - zi).to_double());
        complex delta = g / (fz - 1.0);
        double step_size = std::abs(delta);
        // Converged (as far as the precision goes): the derivatives at z are accurate enough
        if (step_size < 1e-12 || (step_size > last_step * 0.5 && step_size < 1e-6)) {
            double b = (1.0 - std::norm(fz)) / std::abs(fzc + fzz * fc / (1.0 - fz));
            return std::isfinite(b) ? 0.9 * b / 4.0 : 0;
        }
        last_step = step_size;
        zr -= Fixed(delta.real());
        zi -= Fixed(delta.imag());
    }
    return 0;
}

template <int TRAP, typename Abort>
Escape iterate_dd(DD cr, DD ci, int iter_limit, float pixel_size, const Abort& abort) {
    DD zr = {0, 0}, zi = {0, 0}, zr2 = {0, 0}, zi2 = {0, 0};
    float trap = INFINITY;
    TrapScope<float> scope;

    for (int n = 0; n < iter_limit; ++n) {
        if (TRAP != Fractalis::TRAP_OFF) scope.step(static_cast<float>(zr.hi), static_cast<float>(zi.hi), pixel_size);
        DD zri = dd_mul(zr, zi);
        zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
        zr = dd_add(dd_sub(zr2, zi2), cr);
        zr2 = dd_mul(zr, zr);
        zi2 = dd_mul(zi, zi);
        double magnitude_sq = zr2.hi + zi2.hi;
        if (magnitude_sq > BAILOUT_SQ) {
            return {n + 1, static_cast<float>(magnitude_sq), false, false, trap};
        }
        if (TRAP != Fractalis::TRAP_OFF && scope.counts(static_cast<float>(magnitude_sq))) {
            trap = std::min(trap, trap_distance<TRAP>(static_cast<float>(zr.hi), static_cast<float>(zi.hi),
                                                      static_cast<float>(magnitude_sq)));
        }
        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
    }
    return {iter_limit, 0.0f, true, false, trap};
}

/**
 * Perturbation: instead of z, only its difference dz to the reference orbit Z is iterated:
 *   z = Z + dz,  dz' = (2Z + dz) * dz + dc
 * dz and dc are tiny, but single precision keeps their relative precision at any zoom, so this runs on the fast
 * float unit even where z itself would need double-double.
 * Rebasing (Zhuoran): once |z| < |dz| or the reference ends, the pixel continues with dz = z from the start of the
 * reference. That avoids the glitches where the pixel orbit drifts away from the reference.
 * Periodicity check (Brent) on dz: z repeats when Z and dz repeat. Z repeats when the reference is caught in the
 * same cycle (typically inside the same minibrot). dz has the full single precision even where z itself doesn't,
 * it's only compared while small enough that its rounding stays far below epsilon.
 */
template <int TRAP, typename T, typename Abort>
Escape iterate_perturbed(const float* orbit, int orbit_length, T dcr, T dci, int iter_limit, T epsilon,
                         T pixel_size, const Abort& abort) {
    float trap = INFINITY;
    TrapScope<T> scope;
    T dzr = 0, dzi = 0;
    int m = 0;
    float saved_zr = NAN, saved_zi = NAN;
    T saved_dzr = 0, saved_dzi = 0;
    int save_at = 8;
    const T max_compared_dz = epsilon * T(1e6);
    for (int n = 0; n < iter_limit; ++n) {
        if (TRAP != Fractalis::TRAP_OFF) scope.step(orbit[2 * m] + dzr, orbit[2 * m + 1] + dzi, pixel_size);
        T ar = T(2) * orbit[2 * m] + dzr;
        T ai = T(2) * orbit[2 * m + 1] + dzi;
        T nr = ar * dzr - ai * dzi + dcr;
        dzi = ar * dzi + ai * dzr + dci;
        dzr = nr;
        m++;
        T zr = orbit[2 * m] + dzr;
        T zi = orbit[2 * m + 1] + dzi;
        T magnitude_sq = zr * zr + zi * zi;
        if (magnitude_sq > static_cast<T>(BAILOUT_SQ)) {
            return {n + 1, static_cast<float>(magnitude_sq), false, false, trap};
        }
        if (TRAP != Fractalis::TRAP_OFF && scope.counts(magnitude_sq)) {
            trap = std::min(trap, trap_distance<TRAP>(static_cast<float>(zr), static_cast<float>(zi),
                                                      static_cast<float>(magnitude_sq)));
        }
        if (magnitude_sq < dzr * dzr + dzi * dzi || m == orbit_length - 1) {
            dzr = zr;
            dzi = zi;
            m = 0;
        }
        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
        float ref_r = orbit[2 * m], ref_i = orbit[2 * m + 1];
        if (ref_r == saved_zr && ref_i == saved_zi && std::abs(dzr - saved_dzr) + std::abs(dzi - saved_dzi) < epsilon
                && std::abs(dzr) + std::abs(dzi) < max_compared_dz) {
            return {iter_limit, 0.0f, true, false, trap, true};
        }
        if (n == save_at) {
            saved_zr = ref_r;
            saved_zi = ref_i;
            saved_dzr = dzr;
            saved_dzi = dzi;
            save_at *= 2;
        }
    }
    return {iter_limit, 0.0f, true, false, trap};
}

/**
 * Perturbation for deep zooms, where dc and dz are smaller than the smallest float (~1e-38). While dz is tiny, it is
 * stored with its own exponent: dz = w * 2^e with w around 1, and the iteration runs on w:
 *   w' = (2Z + w 2^e) w + dc 2^-e
 * When w grows, e goes up in steps (renormalization) until it reaches 0, from there on it's the plain float
 * iteration. A rebase can make dz tiny again, then e goes down again. All in single precision, only the
 * renormalization (a few times per pixel) uses double.
 */
template <int TRAP, typename Abort>
Escape iterate_perturbed_scaled(const float* orbit, int orbit_length, double dcr, double dci, int iter_limit,
                                double pixel_size, const Abort& abort) {
    constexpr int SHIFT = 16;
    constexpr float UP = 0x1p16f, DOWN = 0x1p-16f;
    float trap = INFINITY;
    TrapScope<double> scope;
    // e is a multiple of SHIFT, it starts where dc is around 1
    const int min_e = std::min(0, std::ilogb(pixel_size) / SHIFT * SHIFT) - 2 * SHIFT;
    int e = min_e + 2 * SHIFT;
    float wr = 0, wi = 0;
    float s, dr, di, tiny_z, epsilon, max_compared;
    double s_double;
    auto set_scale = [&]() {
        s_double = std::ldexp(1.0, e);
        s = e >= -126 ? static_cast<float>(s_double) : 0.0f;  // dz = w * s, 0 where dz is negligible next to Z
        double scale = std::ldexp(1.0, -e);
        dr = static_cast<float>(dcr * scale);
        di = static_cast<float>(dci * scale);
        // Rebasing (|z| < |dz|) is only possible where |Z| < 2 |dz| < 2^(e + SHIFT + 1)
        tiny_z = e + SHIFT + 1 >= -126 ? std::ldexp(1.0f, e + SHIFT + 1) : 0.0f;
        epsilon = static_cast<float>(pixel_size * 1e-3 * scale);
        max_compared = epsilon * 1e6f;
    };
    set_scale();
    int m = 0;
    float saved_zr = NAN, saved_zi = NAN, saved_wr = 0, saved_wi = 0;
    int saved_e = 0;
    int save_at = 8;
    for (int n = 0; n < iter_limit; ++n) {
        float big_zr = orbit[2 * m], big_zi = orbit[2 * m + 1];
        if (TRAP != Fractalis::TRAP_OFF) scope.step(big_zr + wr * s_double, big_zi + wi * s_double, pixel_size);
        float ar = 2.0f * big_zr + wr * s;
        float ai = 2.0f * big_zi + wi * s;
        float nr = ar * wr - ai * wi + dr;
        wi = ar * wi + ai * wr + di;
        wr = nr;
        m++;
        big_zr = orbit[2 * m];
        big_zi = orbit[2 * m + 1];
        float zr = big_zr + wr * s;
        float zi = big_zi + wi * s;
        float magnitude_sq = zr * zr + zi * zi;
        if (magnitude_sq > static_cast<float>(BAILOUT_SQ)) {
            return {n + 1, magnitude_sq, false, false, trap};
        }
        if (TRAP != Fractalis::TRAP_OFF && scope.counts(magnitude_sq)) {
            trap = std::min(trap, trap_distance<TRAP>(zr, zi, magnitude_sq));
        }

        // Rebase
        if (m == orbit_length - 1) {
            // The reference ends, z is not tiny then
            wr = zr;
            wi = zi;
            m = 0;
            e = 0;
            set_scale();
        } else if (e == 0) {
            if (magnitude_sq < wr * wr + wi * wi) {
                wr = zr;
                wi = zi;
                m = 0;
            }
        } else if (std::abs(big_zr) + std::abs(big_zi) < tiny_z) {
            // Compare in units of 2^e: z / 2^e = Z / 2^e + w
            float zwr = std::ldexp(big_zr, -e) + wr;
            float zwi = std::ldexp(big_zi, -e) + wi;
            if (zwr * zwr + zwi * zwi < wr * wr + wi * wi) {
                wr = zwr;
                wi = zwi;
                m = 0;
            }
        }

        // Renormalize
        float size = std::abs(wr) + std::abs(wi);
        if (e < 0 && size > UP) {
            do {
                wr *= DOWN;
                wi *= DOWN;
                e += SHIFT;
                size *= DOWN;
            } while (e < 0 && size > UP);
            set_scale();
        } else if (size < (e == 0 ? 0x1p-90f : DOWN) && size > 0 && e > min_e) {
            do {
                wr *= UP;
                wi *= UP;
                e -= SHIFT;
                size *= UP;
            } while (size < DOWN && e > min_e);
            set_scale();
        }

        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
        float ref_r = orbit[2 * m], ref_i = orbit[2 * m + 1];
        if (ref_r == saved_zr && ref_i == saved_zi && e == saved_e
                && std::abs(wr - saved_wr) + std::abs(wi - saved_wi) < epsilon && size < max_compared) {
            return {iter_limit, 0.0f, true, false, trap, true};
        }
        if (n == save_at) {
            saved_zr = ref_r;
            saved_zi = ref_i;
            saved_wr = wr;
            saved_wi = wi;
            saved_e = e;
            save_at *= 2;
        }
    }
    return {iter_limit, 0.0f, true, false, trap};
}

}  // namespace

Fractalis::Fractalis(FractalisState* state)
    : state(state), pass_id(0), pass_limit(0), pass_target(0), pass_resolved(0), next_index(0), in_flight(0),
      returned_count(0), first_limit_hint(0) {
    ref = {};
    // Z_0 .. Z_MAX_ITER
    ref.orbit = new float[2 * (MAX_ITER + 1)];

    // Probe points: the center 3x3 at every pixel, then 9x9 points at every 2nd pixel, then rings of 9x9 points
    // with twice the distance each, until they cover the screen
    probe_count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) probes[probe_count++] = {static_cast<int16_t>(dx), static_cast<int16_t>(dy)};
    }
    const int half = std::max(state->screen_w, state->screen_h) / 2;
    for (int step = 2; 2 * step <= half && probe_count < MAX_PROBES; step *= 2) {
        // The inner points are already there from the finer level before
        const int inner = step == 2 ? 0 : 2;
        for (int j = -4; j <= 4; ++j) {
            for (int i = -4; i <= 4; ++i) {
                if (std::abs(i) <= inner && std::abs(j) <= inner) continue;
                if (std::abs(i * step) * 2 > state->screen_w || std::abs(j * step) * 2 > state->screen_h) continue;
                if (probe_count < MAX_PROBES) {
                    probes[probe_count++] = {static_cast<int16_t>(i * step), static_cast<int16_t>(j * step)};
                }
            }
        }
    }
}

Fractalis::~Fractalis() {
    delete[] ref.orbit;
}

uint64_t (*Fractalis::clock_us)() = nullptr;

void Fractalis::log_pass(const char* name) {
    if (!clock_us) return;
    int left = static_cast<int>(sizeof(stats.passes)) - stats.passes_length;
    if (left <= 1) return;
    int n = snprintf(stats.passes + stats.passes_length, left, "%s%d:%lums/%dpx ", name, pass_limit,
                     static_cast<unsigned long>((now_us() - stats.pass_start) / 1000), stats.pass_pixels);
    stats.passes_length += std::max(0, std::min(n, left - 1));
}

void Fractalis::print_stats() {
    if (!clock_us) return;
    printf("Stats: %lu ms | %s| reference: %d orbits, %d iterations, %lu ms, search %lu ms\n",
           static_cast<unsigned long>((now_us() - stats.view_start) / 1000), stats.passes, stats.ref_orbits,
           stats.ref_iterations, static_cast<unsigned long>(stats.ref_us / 1000),
           static_cast<unsigned long>(stats.search_us / 1000));
}

int Fractalis::max_iterations(double zoom) const {
    double scale = state->screen_w / (3.0 / zoom);
    int max_iter = static_cast<int>(50 * std::pow(std::log10(scale), 1.25));
    return std::min(max_iter, MAX_ITER);
}

bool Fractalis::calculate_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                                PixelState& pixel, float sub_x, float sub_y, PixelInfo* info) const {
    auto abort = [this, id, interrupt]() {
        return state->calculation_id != id || (interrupt && interrupt());
    };
    // The interior checks know that a pixel is in the set without iterating. An orbit trap needs the orbit though,
    // a short one is enough: it is caught in its cycle quickly.
    const bool trapped = view.trap != TRAP_OFF;
    const int interior_limit = std::min(iter_limit, TRAP_INTERIOR_ITER);
    Escape escape;

    if (view.zoom < FLOAT_MAX_ZOOM) {
        // Everything in single precision, double math is slower and would cost more than the iterations
        float cr = view.center_rf + ((x + 0.5f + sub_x - state->screen_w / 2.0f) * view.step_f + view.center_rf_low);
        float ci = view.center_if + ((y + 0.5f + sub_y - state->screen_h / 2.0f) * view.step_f + view.center_if_low);
        bool interior = is_in_main_bulb(cr, ci);
        if (interior && !trapped) {
            escape = {iter_limit, 0.0f, true, false, INFINITY, true};
        } else {
            escape = with_trap(view.trap, [&](auto trap) {
                return iterate<decltype(trap)::value>(cr, ci, interior ? interior_limit : iter_limit,
                                                      view.step_f * 1e-3f, true, view.step_f, abort);
            });
            if (interior) escape.in_set = escape.proven = true;
        }
    } else {
        // Offset from the center in the complex plane. Exact enough in double, even for deep zooms.
        double offset_r = (x + 0.5 + sub_x - state->screen_w / 2.0) * view.step;
        double offset_i = (y + 0.5 + sub_y - state->screen_h / 2.0) * view.step;
        double cr = view.center_r + offset_r;
        double ci = view.center_i + offset_i;
        bool optimize = view.zoom < OPTIMIZATIONS_MAX_ZOOM;

        bool interior = optimize && is_in_main_bulb(cr, ci);
        if (view.perturbed && !interior) {
            double ref_r = view.ref_offset_r + offset_r;
            double ref_i = view.ref_offset_i + offset_i;
            double nucleus_r = view.nucleus_offset_r + offset_r;
            double nucleus_i = view.nucleus_offset_i + offset_i;
            interior = ref_r * ref_r + ref_i * ref_i < view.interior_radius_sq
                    || (view.minibrot && is_in_component(view.cardioid,
                            static_cast<float>(nucleus_r * view.scale_r - nucleus_i * view.scale_i),
                            static_cast<float>(nucleus_r * view.scale_i + nucleus_i * view.scale_r)));
        }
        int limit = interior ? interior_limit : iter_limit;

        if (interior && !trapped) {
            escape = {iter_limit, 0.0f, true, false, INFINITY, true};
        } else if (view.perturbed) {
            double dcr = view.ref_offset_r + offset_r;
            double dci = view.ref_offset_i + offset_i;
            escape = with_trap(view.trap, [&](auto trap) {
#ifdef DOUBLE_PERTURBATION
                // For tests: the reference for the single precision versions
                return iterate_perturbed<decltype(trap)::value>(view.orbit, view.orbit_length, dcr, dci, limit,
                                                                view.step * 1e-3, view.step, abort);
#else
                if (view.zoom >= SCALED_PERTURBATION_MIN_ZOOM) {
                    return iterate_perturbed_scaled<decltype(trap)::value>(view.orbit, view.orbit_length, dcr, dci,
                                                                           limit, view.step, abort);
                }
                return iterate_perturbed<decltype(trap)::value>(view.orbit, view.orbit_length,
                                                                static_cast<float>(dcr), static_cast<float>(dci),
                                                                limit, static_cast<float>(view.step * 1e-3),
                                                                view.step_f, abort);
#endif
            });
        } else if (view.zoom < DOUBLE_MAX_ZOOM) {
            escape = with_trap(view.trap, [&](auto trap) {
                return iterate<decltype(trap)::value>(cr, ci, limit, view.step * 1e-3, optimize, view.step_f, abort);
            });
        } else {
            DD re = dd_add({view.center_r, view.center_r_low}, {offset_r, 0});
            DD im = dd_add({view.center_i, view.center_i_low}, {offset_i, 0});
            escape = with_trap(view.trap, [&](auto trap) {
                return iterate_dd<decltype(trap)::value>(re, im, limit, view.step_f, abort);
            });
        }
        if (interior) escape.in_set = escape.proven = true;
    }

    if (escape.aborted) {
        return false;
    }
    if (info) {
        info->iterations = escape.iterations;
        info->ran_out = escape.in_set && !escape.proven;
    }

    if (trapped) {
        // Color by the distance of the orbit to the trap, inside the set as well
        uint32_t position = palette::trap_position(escape.trap);
        if (escape.in_set) {
            pixel.setInSetColored(position);
        } else {
            pixel.setEscaped(position);
        }
    } else if (escape.in_set) {
        pixel.setInSet();
    } else {
        // Smooth (continuous) iteration count. The +2 (instead of the usual +1) keeps the palette as it was.
        float log2_z = 0.5f * std::log2(escape.magnitude_sq);
        float smooth = escape.iterations + 2 - std::log2(log2_z);
        pixel.setEscaped(palette::position(smooth));
    }
    return true;
}

bool Fractalis::supersample_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                                  PixelState& pixel) const {
    // The pixel center is the first sample, the others around it
    static constexpr float OFFSETS_2[][2] = {{0.35f, 0.35f}};                     // diagonal
    static constexpr float OFFSETS_3[][2] = {{0.38f, -0.22f}, {-0.38f, 0.22f}};   // line through the center
    // Regular rings with radius 0.42 around the center
    static constexpr float OFFSETS_4[][2] = {{0.0f, -0.42f}, {-0.36f, 0.21f}, {0.36f, 0.21f}};
    static constexpr float OFFSETS_6[][2] = {
        {0.000f, -0.420f}, {0.399f, -0.130f}, {0.247f, 0.340f}, {-0.247f, 0.340f}, {-0.399f, -0.130f}};
    static constexpr float OFFSETS_8[][2] = {
        {0.000f, -0.420f}, {0.328f, -0.262f}, {0.409f, 0.093f}, {0.182f, 0.378f}, {-0.182f, 0.378f},
        {-0.409f, 0.093f}, {-0.328f, -0.262f}};
    const float (*offsets)[2] = ss_samples == 2 ? OFFSETS_2 : ss_samples == 3 ? OFFSETS_3
                              : ss_samples == 4 ? OFFSETS_4 : ss_samples == 6 ? OFFSETS_6 : OFFSETS_8;
    int in_set = 0;
    int black = 0;    // in the set and not colored (without orbit trap)
    int colored = 0;
    uint64_t sum = 0;
    uint32_t lowest = UINT32_MAX, highest = 0;
    auto add = [&](const PixelState& sample) {
        if (sample.isInSet()) in_set++;
        if (!sample.showsColor()) {
            black++;
            return;
        }
        uint32_t position = sample.position();
        colored++;
        sum += position;
        lowest = std::min(lowest, position);
        highest = std::max(highest, position);
    };
    add(pixel);
    for (int i = 0; i < ss_samples - 1; ++i) {
        PixelState sample;
        const float* offset = offsets[i];
        if (!calculate_pixel(x, y, view, iter_limit, id, interrupt, sample, offset[0], offset[1])) {
            return false;
        }
        add(sample);
    }

    if (colored == 0) {
        pixel.setInSet();
        pixel.setSupersampled(1, 0);
    } else {
        uint32_t mean = static_cast<uint32_t>(sum / colored);
        if (in_set == ss_samples) {
            pixel.setInSetColored(mean);  // orbit trap inside the set
        } else {
            pixel.setEscaped(mean);
        }
        // Share of the black samples in quarters. All of them would be black.
        int coverage = std::min(3, (4 * black + ss_samples / 2) / ss_samples);
        pixel.setSupersampled(palette::spread_level(highest - lowest), static_cast<uint8_t>(coverage));
    }
    return true;
}

bool Fractalis::needs_work(const PixelState& pixel) const {
    return ss_pass ? pixel.isComplete() && !pixel.isSupersampled() : !pixel.isComplete();
}

bool Fractalis::claim_pixel(int& x, int& y) {
    // Pixels an interrupted core gave back come first
    while (returned_count > 0) {
        returned_count--;
        x = returned[returned_count].x;
        y = returned[returned_count].y;
        if (probe_pass || needs_work(state->pixelState[y][x])) {
            return true;
        }
    }

    if (probe_pass) {
        const int cx = state->screen_w / 2;
        const int cy = state->screen_h / 2;
        // All of them, also the ones that are already done: their iterations are needed
        while (next_index < probe_count) {
            const PixelPosition& p = probes[next_index++];
            x = cx + p.x;
            y = cy + p.y;
            if (x >= 0 && x < state->screen_w && y >= 0 && y < state->screen_h) return true;
        }
        return false;
    }

    while (next_in_order(x, y)) {
        if (needs_work(state->pixelState[y][x])) return true;
    }
    return false;
}

bool Fractalis::next_in_order(int& x, int& y) {
    // Every pixel is on a grid around the screen center: the coarsest one of 64, 32, .. 2, 1 pixels that it is on.
    // In grid units (i, j) it is on the square ring p = max(|i|, |j|). Pixels are handed out by p, the coarser grids
    // first, so the whole screen is quickly covered by a coarse grid while the full resolution grows from the center.
    static constexpr int STEPS[] = {64, 32, 16, 8, 4, 2, 1};
    constexpr int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
    const int cx = state->screen_w / 2;
    const int cy = state->screen_h / 2;
    const int max_radius = std::max(std::max(cx, state->screen_w - cx), std::max(cy, state->screen_h - cy));
    if (order_p == 0) {
        order_p = 1;
        order_step = 0;
        order_k = 0;
        x = cx;
        y = cy;
        return true;
    }
    while (order_p <= max_radius) {
        if (order_step == STEP_COUNT) {
            order_p++;
            order_step = 0;
            order_k = 0;
            continue;
        }
        const int s = STEPS[order_step];
        if (order_k >= 8 * order_p || order_p * s > max_radius) {
            // ring done or completely outside of the screen
            order_step++;
            order_k = 0;
            continue;
        }
        int k = order_k++;
        int side = k / (2 * order_p), pos = k % (2 * order_p);
        int i, j;
        switch (side) {
            case 0: i = -order_p + pos; j = -order_p; break;
            case 1: i = order_p; j = -order_p + pos; break;
            case 2: i = order_p - pos; j = order_p; break;
            default: i = -order_p; j = order_p - pos; break;
        }
        // On a coarser grid as well: that one has it
        if (s < STEPS[0] && i % 2 == 0 && j % 2 == 0) continue;
        x = cx + i * s;
        y = cy + j * s;
        if (x >= 0 && x < state->screen_w && y >= 0 && y < state->screen_h) return true;
    }
    return false;
}

void Fractalis::start_pass() {
    // A new view: start with a low iteration limit, so a first image appears quickly
    pass_id = state->calculation_id;
    ss_pass = false;
    state->supersampling = false;
    pass_view.center = state->center;
    pass_view.zoom = state->zoom_factor;
    pass_view.step = 4.0 / state->zoom_factor / state->screen_w;
    pass_view.center_r = pass_view.center.real.to_double();
    pass_view.center_i = pass_view.center.imag.to_double();
    pass_view.center_r_low = (pass_view.center.real - Fixed(pass_view.center_r)).to_double();
    pass_view.center_i_low = (pass_view.center.imag - Fixed(pass_view.center_i)).to_double();
    pass_view.center_rf = static_cast<float>(pass_view.center_r);
    pass_view.center_if = static_cast<float>(pass_view.center_i);
    pass_view.center_rf_low = static_cast<float>((pass_view.center.real - Fixed(pass_view.center_rf)).to_double());
    pass_view.center_if_low = static_cast<float>((pass_view.center.imag - Fixed(pass_view.center_if)).to_double());
    pass_view.step_f = static_cast<float>(pass_view.step);
    pass_view.trap = trap_mode;
    pass_view.perturbed = pass_view.zoom >= PERTURBATION_MIN_ZOOM;

    returned_count = 0;

    // Keep the reference orbit while C is close to the screen. Further away, the pixel distances get lost in the
    // single precision dc.
    bool keep_reference = ref.length > 0 || ref.busy;
    if (keep_reference) {
        double dx = (ref.c.real - pass_view.center.real).to_double() / pass_view.step;
        double dy = (ref.c.imag - pass_view.center.imag).to_double() / pass_view.step;
        keep_reference = std::abs(dx) < state->screen_w && std::abs(dy) < state->screen_w;
    }

    stats = {};
    stats.view_start = now_us();

    // First the probe points, they set the target of the view (begin_view_passes). Right after a pan or zoom twice
    // the limit of the view before is enough, otherwise the highest limit.
    probe_pass = true;
    probe_escaped = 0;
    probe_ran_out = 0;
    int probe_limit = MAX_ITER;
    if (first_limit_hint > 0 && last_view_limit > 0) {
        probe_limit = std::min(MAX_ITER, 2 * std::max(last_view_limit, max_iterations(pass_view.zoom)));
    }
    view_target = probe_limit;
    begin_pass(probe_limit, probe_limit);
    // (begin_pass already chose a new one, if the kept orbit is too short)
    if (pass_view.perturbed && !keep_reference && ref.tries == 0) {
        choose_reference();
    }
}

int Fractalis::probe_percentile(float share) const {
    // probe_results is sorted by iterations
    int total = 0;
    for (int i = 0; i < probe_escaped; ++i) total += probe_results[i].weight;
    int sum = 0;
    for (int i = 0; i < probe_escaped; ++i) {
        sum += probe_results[i].weight;
        if (sum >= share * total) return probe_results[i].iterations;
    }
    return probe_escaped > 0 ? probe_results[probe_escaped - 1].iterations : 0;
}

void Fractalis::begin_view_passes() {
    // The target: most escaping probes are done there. Probes that didn't escape even at the highest limit are
    // in the set as far as this program can tell, they don't need a higher target.
    int target;
    int p95 = 0;
    if (probe_escaped > 0) {
        std::sort(probe_results, probe_results + probe_escaped,
                  [](const ProbeResult& a, const ProbeResult& b) { return a.iterations < b.iterations; });
        p95 = probe_percentile(0.95f);
        target = p95 + p95 / 4;
    } else {
        target = max_iterations(pass_view.zoom);
    }
    // At least the estimate from the zoom: where the probes escape early (zoomed out), it still shows the border of
    // the set with enough detail
    target = std::max(target, max_iterations(pass_view.zoom));
    target = std::max(2 * FIRST_PASS_ITER, std::min(target, MAX_ITER));
    view_target = target;
    printf("Probes: %d escaped (95%% by %d), %d undecided -> target %d\n", probe_escaped, p95, probe_ran_out,
           target);

    int first;
    if (first_limit_hint > 0) {
        // Most pixels escape around the hint, lower passes would only show the preview again
        first = std::max(FIRST_PASS_ITER, std::min(first_limit_hint, target));
        if (first * 3 / 2 >= target) first = target;
    } else if (probe_escaped > 0) {
        // Below the point where the first few percent of the probes escape, a pass would only draw black
        first = std::max(FIRST_PASS_ITER, std::min(probe_percentile(0.05f), target));
        if (first * 3 / 2 >= target) first = target;
    } else {
        first = next_pass_limit(std::max(FIRST_PASS_ITER, target / 16) / 2, target);
    }
    begin_pass(first, target);
}

int Fractalis::estimate_first_limit() const {
    // Orbit trap positions say nothing about the iterations
    if (trap_mode != TRAP_OFF) return 0;
    // From the distribution of the smooth iteration counts on screen, a histogram of the palette positions
    // (t = log(1 + smooth) / 2, the upper 8 bit of PixelState::color are t * 16)
    constexpr int BINS = 128;
    uint16_t histogram[BINS] = {};
    int count = 0;
    for (int y = 0; y < state->screen_h; y += 2) {
        for (int x = 0; x < state->screen_w; x += 2) {
            const PixelState& p = state->pixelState[y][x];
            if (!p.hasPosition()) continue;
            histogram[std::min(p.color >> 8, BINS - 1)]++;
            count++;
        }
    }
    if (count < 100) return 0;
    int bin = 0;
    // Where 3/4 of the pixels escaped: the first pass shows most of the image, only a quarter is calculated again
    for (int sum = 0; bin < BINS - 1 && (sum += histogram[bin]) < count * 3 / 4; ++bin) {}
    float t = (bin + 1) / 16.0f;
    float smooth = std::exp(2.0f * t) - 1.0f;
    return static_cast<int>(smooth * 1.25f);
}

int Fractalis::next_pass_limit(int limit, int target) const {
    // Doubling, but a tiny last step isn't worth recalculating all undecided pixels
    int next = limit * 2;
    return next * 3 / 2 >= target ? target : next;
}

void Fractalis::begin_pass(int limit, int target) {
    pass_limit = limit;
    pass_target = target;
    pass_resolved = 0;
    pass_ran_out = 0;
    next_index = 0;
    order_p = 0;
    stats.pass_start = now_us();
    stats.pass_pixels = 0;
    if (!probe_pass) state->iteration_limit = limit;
    if (pass_view.perturbed) {
        ref.tries = 0;
        ref.best_length = 0;
        if (ref.escaped && ref.length <= limit && !probe_pass) {
            // The reference escapes too early for this pass, take one of the pixels that are still undecided. Not
            // for the probes: they only measure, rebasing covers the rest of their orbit.
            choose_reference();
        }
    }
    printf("Starting %s %lu with iteration limit %d\n", probe_pass ? "probes of view" : "pass",
           static_cast<unsigned long>(pass_id), limit);
}

void Fractalis::finish_pass() {
    if (ss_pass) {
        ss_pass = false;
        state->supersampling = false;
        state->calculating = 0;
        state->passes_completed++;
        state->needs_redraw = true;
        printf("Supersampling complete\n");
        log_pass("ss");
        print_stats();
        return;
    }
    if (probe_pass) {
        log_pass("probes");
        probe_pass = false;
        begin_view_passes();
        state->needs_redraw = true;
        return;
    }

    // Count the pixels that are still undecided. Supersampled right away: also the ones with sub-samples in the set,
    // a higher limit can change their share.
    const bool right_away = right_away_pass();
    auto undecided = [right_away](const PixelState& p) { return p.isInSet() || (right_away && p.coverage() > 0); };
    int unresolved = 0;
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            if (undecided(state->pixelState[y][x])) unresolved++;
        }
    }

    int next_limit = 0;
    if (unresolved > 0) {
        if (pass_limit < pass_target) {
            next_limit = next_pass_limit(pass_limit, pass_target);
        } else {
            // Keep refining while the higher limits still reveal new details, or while a big part of the screen
            // reached the limit without being proven to be in the set and they still escape with higher limits
            bool unexplained = pass_ran_out * 4 > state->screen_w * state->screen_h && pass_resolved * 100 >= pass_ran_out;
            int max_target = unexplained ? MAX_ITER : std::min(MAX_ITER, view_target * REFINE_MAX_FACTOR);
            if (pass_limit < max_target
                    && (unexplained || (!state->auto_zoom && pass_resolved >= REFINE_MIN_PIXELS))) {
                pass_target = std::min(pass_limit * 2, max_target);
                next_limit = pass_target;
            }
        }
    }

    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            PixelState& pixel = state->pixelState[y][x];
            if (!undecided(pixel)) continue;
            if (next_limit) {
                // Pixels that escaped keep their value with a higher limit, only these need another pass
                pixel.markIncomplete();
            } else if (pixel.isInSet()) {
                // Final: in the set, no more preview colors. With an orbit trap they are the trap colors.
                if (pass_view.trap == TRAP_OFF) pixel.dropPreviewColor();
            }
        }
    }

    state->completed_limit = static_cast<uint16_t>(pass_limit);
    log_pass("");
    if (next_limit) {
        begin_pass(next_limit, pass_target);
    } else {
        printf("Calculation complete at iteration limit %d\n", pass_limit);
        last_view_limit = pass_limit;
        if (ss_samples > 1 && !right_away && (!state->auto_zoom || state->auto_zoom_full_quality)) {
            begin_supersampling();
        } else {
            state->calculating = 0;
            print_stats();
        }
    }
    state->passes_completed++;
    state->needs_redraw = true;
}

void Fractalis::store_result(int x, int y, const PixelState& result, const PixelInfo& info) {
    if (probe_pass) {
        if (info.ran_out) {
            probe_ran_out++;
        } else if (!result.isInSet() && probe_escaped < MAX_PROBES) {
            // The grid spacing of the probe ring it is on (see the constructor)
            int distance = std::max(std::abs(x - state->screen_w / 2), std::abs(y - state->screen_h / 2));
            int spacing = 1;
            if (distance > 1) {
                spacing = 2;
                while (spacing * 4 < distance) spacing *= 2;
            }
            probe_results[probe_escaped++] = {static_cast<uint16_t>(std::min(info.iterations, 65535)),
                                              static_cast<uint16_t>(spacing * spacing)};
        }
        // Only measuring: the passes draw the pixel
        if (!show_probes_) return;
    } else if (!ss_pass && info.ran_out) {
        pass_ran_out++;
    }
    stats.pass_pixels++;
    PixelState& target = state->pixelState[y][x];
    if (ss_pass) {
        target = result;
    } else if (!undecided_in_set_ && result.isInSet() && !result.showsColor() && info.ran_out
               && target.showsColor()) {
        // Undecided at this iteration limit: keep showing the zoom preview instead of black for now. Pixels that are
        // proven to be in the set are black right away.
        target.setInSetKeepingPreview();
    } else if (!undecided_in_set_ && result.isInSet() && !result.showsColor() && info.ran_out
               && !target.isValid()) {
        // Undecided, nothing to show yet: blended from the calculated points around it as preview, if most of them
        // are colored. Black only once the last pass confirms it.
        uint32_t position;
        Blend blend = interpolated_position(state->pixelState, state->screen_w, state->screen_h, x, y, position);
        const PixelState* near = blend == Blend::NONE
            ? nearest_calculated(state->pixelState, state->screen_w, state->screen_h, x, y, false) : nullptr;
        if (blend == Blend::COLOR) {
            target.setInSetColored(position);
        } else if (near && near->showsColor()) {
            target.setInSetColored(near->position());
        } else {
            target = result;
        }
    } else if (right_away_pass()) {
        // Pixels with sub-samples in the set are calculated again in the next pass. Like without supersampling, they
        // only count as resolved when they were completely in the set before.
        if (!result.isInSet() && (target.isInSet() || !target.isValid())) pass_resolved++;
        target = result;
    } else {
        target = result;
        if (!result.isInSet()) pass_resolved++;
    }
}

bool Fractalis::reference_ready() const {
    if (ref.busy || ref.length < 2) return false;
    // An orbit that escaped too early is still used once no better C was found
    return ref.length > pass_limit || ref.escaped;
}

void Fractalis::set_reference(const Coordinate& c) {
    ref.c = c;
    ref.length = 0;
    ref.escaped = false;
    ref.generation++;
    ref.minibrot = Reference::MINIBROT_UNKNOWN;
    ref.minibrot_tried_length = 0;
}

void Fractalis::choose_reference() {
    const int cx = state->screen_w / 2;
    const int cy = state->screen_h / 2;
    int best_x = cx, best_y = cy;
    int best_distance = INT32_MAX;
    uint32_t best_position = 0;
    bool in_set_found = false;
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            const PixelState& p = state->pixelState[y][x];
            bool tried = false;
            for (int i = 0; i < std::min(ref.tries, static_cast<int>(Reference::MAX_TRIES)); ++i) {
                tried |= ref.tried_x[i] == x && ref.tried_y[i] == y;
            }
            if (tried) continue;
            if (p.isInSet()) {
                int distance = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (!in_set_found || distance < best_distance) {
                    in_set_found = true;
                    best_distance = distance;
                    best_x = x;
                    best_y = y;
                }
            } else if (!in_set_found && p.hasPosition() && p.position() > best_position) {
                best_position = p.position();
                best_x = x;
                best_y = y;
            }
        }
    }
    if (ref.tries < Reference::MAX_TRIES) {
        ref.tried_x[ref.tries] = static_cast<int16_t>(best_x);
        ref.tried_y[ref.tries] = static_cast<int16_t>(best_y);
    }
    ref.tries++;
    // The pixel center, like calculate_pixel()
    Coordinate c = pass_view.center;
    Fixed half_step(pass_view.step / 2);
    c.real += half_step.times(2 * best_x + 1 - state->screen_w);
    c.imag += half_step.times(2 * best_y + 1 - state->screen_h);
    set_reference(c);
}

bool Fractalis::extend_reference(uint32_t id, int target_length, bool (*interrupt)()) {
    uint32_t generation;
    Fixed cr, ci, zr, zi;
    int n;
    {
        LockGuard guard(lock);
        generation = ref.generation;
        cr = ref.c.real;
        ci = ref.c.imag;
        zr = ref.zr;
        zi = ref.zi;
        n = ref.length;
    }

    const uint64_t started = now_us();
    const int first_n = n;
    // Nobody reads the orbit while it is not ready
    float* orbit = ref.orbit;
    if (n == 0) {
        orbit[0] = orbit[1] = 0.0f;
        zr = zi = 0.0;
        n = 1;
    }
    bool escaped = false;
    bool aborted = false;
    while (n < target_length) {
        square_add(zr, zi, cr, ci);
        float fr = static_cast<float>(zr.to_double());
        float fi = static_cast<float>(zi.to_double());
        orbit[2 * n] = fr;
        orbit[2 * n + 1] = fi;
        n++;
        // Escaped. Not further: the fixed point range ends at 128. Pixels rebase at the end of the orbit.
        if (fr * fr + fi * fi > 4.0f) {
            escaped = true;
            break;
        }
        if ((n & 255) == 0 && (state->calculation_id != id || (interrupt && interrupt()))) {
            aborted = true;
            break;
        }
    }

    // Period guess for the minibrot search: the orbit comes closest to 0 at multiples of the period. The search
    // reduces a multiple to the actual period.
    int period_guess = 0;
    bool search_minibrot = false;
    if (!escaped && n >= 64) {
        float lowest = INFINITY;
        for (int k = 1; k < n; ++k) {
            float m = orbit[2 * k] * orbit[2 * k] + orbit[2 * k + 1] * orbit[2 * k + 1];
            if (m < lowest) {
                lowest = m;
                period_guess = k;
            }
        }
    }

    {
        LockGuard guard(lock);
        ref.busy = false;
        stats.ref_us += now_us() - started;
        stats.ref_iterations += n - first_n;
        if (first_n == 0) stats.ref_orbits++;
        if (ref.generation != generation) {
            return true;  // a new C was chosen in the mean time
        }
        // Also an aborted orbit is valid as far as it got
        ref.length = n;
        ref.escaped = escaped;
        ref.zr = zr;
        ref.zi = zi;

        if (escaped && n <= pass_limit && id == pass_id && pass_view.perturbed && !probe_pass) {
            // Pixels that need more iterations than the reference has would lose their precision
            if (n > ref.best_length) {
                ref.best_length = n;
                ref.best_c = ref.c;
            }
            if (ref.tries < Reference::MAX_TRIES) {
                choose_reference();
            } else if (ref.best_length > n) {
                set_reference(ref.best_c);
            }
        } else if (period_guess > 1 && (ref.minibrot == Reference::MINIBROT_UNKNOWN
                   || (ref.minibrot == Reference::MINIBROT_NONE && n >= 2 * ref.minibrot_tried_length))) {
            ref.minibrot = Reference::MINIBROT_NONE;
            ref.minibrot_tried_length = n;
            search_minibrot = true;
        }
    }
    if (!search_minibrot) {
        return !aborted;
    }

    // Takes a few thousand fixed point iterations, the orbit is already usable in the mean time
    bool interrupted = false;
    auto check_interrupt = [&]() {
        return interrupted = interrupt && interrupt();
    };
    const uint64_t search_started = now_us();
    Minibrot minibrot;
    bool found = find_minibrot(cr, ci, period_guess, minibrot, check_interrupt);
    // The last orbit value is close to the attracting cycle
    double radius = found && !interrupted ? interior_radius(zr, zi, cr, ci, minibrot.period, check_interrupt) : 0;

    LockGuard guard(lock);
    stats.search_us += now_us() - search_started;
    if (ref.generation == generation) {
        if (interrupted) {
            ref.minibrot = Reference::MINIBROT_UNKNOWN;  // try again
        } else if (found && (minibrot.usable || radius > 0)) {
            ref.minibrot = Reference::MINIBROT_FOUND;
            ref.component_check = minibrot.usable;
            ref.period = minibrot.period;
            ref.nucleus = {minibrot.nr, minibrot.ni};
            ref.cardioid = minibrot.cardioid;
            ref.scale_r = minibrot.scale_r;
            ref.scale_i = minibrot.scale_i;
            ref.interior_radius = radius;
            printf("Reference in a %s of period %d, size %.3g, inside within %.3g\n",
                   minibrot.cardioid ? "minibrot" : "bulb", minibrot.period,
                   1.0 / std::hypot(minibrot.scale_r, minibrot.scale_i), radius);
        }
    }
    return !aborted && !interrupted;
}

bool Fractalis::work(bool (*interrupt)()) {
    if (state->calculating == 0) {
        return false;
    }

    // Pixels are claimed in small batches, to keep the locking overhead low for fast pixels
    int xs[MAX_BATCH], ys[MAX_BATCH];
    PixelState results[MAX_BATCH];
    PixelInfo infos[MAX_BATCH] = {};
    int count = 0;
    uint32_t id;
    View view;
    int iter_limit;
    bool calculate_reference = false;
    bool supersampling = false;
    bool right_away = false;
    {
        LockGuard guard(lock);
        if (state->calculating == 0) {
            return false;
        }
        if (pass_id != state->calculation_id) {
            start_pass();
        }
        if (pass_view.perturbed && !reference_ready()) {
            // Wait for the other core: it is calculating the orbit or still has pixels of the previous view, which
            // read the orbit until they notice that they are not needed anymore
            if (ref.busy || in_flight > 0) {
                return false;
            }
            ref.busy = true;
            calculate_reference = true;
            id = pass_id;
            // Straight to the limit of the view: costs little compared to the pixels, and the component search
            // needs an orbit longer than the period
            iter_limit = std::max(pass_limit, pass_target);
        }
    }
    if (calculate_reference) {
        return extend_reference(id, iter_limit + 1, interrupt);
    }
    {
        LockGuard guard(lock);
        if (state->calculating == 0 || pass_id != state->calculation_id
                || (pass_view.perturbed && !reference_ready())) {
            return true;  // changed in the mean time, try again
        }
        int batch = pass_view.zoom < FLOAT_MAX_ZOOM ? MAX_BATCH
                  : pass_view.perturbed ? 8
                  : pass_view.zoom < DOUBLE_MAX_ZOOM ? 4 : 1;
        if (ss_pass || right_away_pass()) {
            batch = std::max(1, batch / (ss_samples - 1));  // every pixel is several samples
        }
        right_away = right_away_pass();
        supersampling = ss_pass;
        while (count < batch && claim_pixel(xs[count], ys[count])) {
            if (supersampling) {
                results[count] = state->pixelState[ys[count]][xs[count]];  // the center sample
            }
            count++;
        }
        if (count == 0) {
            if (in_flight == 0) {
                finish_pass();
                return state->calculating != 0;
            }
            return false;  // the other core is still busy with the last pixels
        }
        id = pass_id;
        view = pass_view;
        iter_limit = pass_limit;
        if (view.perturbed) {
            view.ref_offset_r = (view.center.real - ref.c.real).to_double();
            view.ref_offset_i = (view.center.imag - ref.c.imag).to_double();
            view.orbit = ref.orbit;
            view.orbit_length = ref.length;
            view.minibrot = ref.minibrot == Reference::MINIBROT_FOUND && ref.component_check;
            view.interior_radius_sq = ref.minibrot == Reference::MINIBROT_FOUND
                                    ? ref.interior_radius * ref.interior_radius : 0;
            if (view.minibrot) {
                view.nucleus_offset_r = (view.center.real - ref.nucleus.real).to_double();
                view.nucleus_offset_i = (view.center.imag - ref.nucleus.imag).to_double();
                view.cardioid = ref.cardioid;
                view.scale_r = ref.scale_r;
                view.scale_i = ref.scale_i;
            }
        }
        in_flight++;
    }

    int done = 0;
    while (done < count) {
        bool calculated = supersampling
            ? supersample_pixel(xs[done], ys[done], view, iter_limit, id, interrupt, results[done])
            : calculate_pixel(xs[done], ys[done], view, iter_limit, id, interrupt, results[done], 0.0f, 0.0f,
                              &infos[done]);
        // Right away: the other sub-samples as well, the pixel center is the first one
        if (calculated && right_away) {
            calculated = supersample_pixel(xs[done], ys[done], view, iter_limit, id, interrupt, results[done]);
        }
        if (!calculated) break;
        done++;
    }

    LockGuard guard(lock);
    in_flight--;
    // Throw the results away, if the view changed in the mean time
    if (id != state->calculation_id) {
        return true;
    }
    for (int i = 0; i < done; ++i) {
        store_result(xs[i], ys[i], results[i], infos[i]);
    }
    // Interrupted: give the rest back, so they are calculated by the next free core
    for (int i = done; i < count && returned_count < RETURNED_CAPACITY; ++i) {
        returned[returned_count++] = {static_cast<int16_t>(xs[i]), static_cast<int16_t>(ys[i])};
    }
    return done == count;
}

void Fractalis::request_calculation() {
    state->completed_limit = 0;
    state->supersampling = false;
    state->calculating = 1;
    state->calculation_id++;
    state->needs_redraw = true;
}

void Fractalis::zoom(double factor) {
    LockGuard guard(lock);
    double old_zoom = state->zoom_factor;
    state->zoom_factor *= 1.0 + factor;
    state->scalePixelState(old_zoom / state->zoom_factor);
    first_limit_hint = estimate_first_limit();
    request_calculation();
    printf("Zooming. New Zoom Factor: %f\n", state->zoom_factor);
}

void Fractalis::pan(double dx, double dy) {
    int shift_x = static_cast<int>(std::lround(dx * state->screen_w));
    int shift_y = static_cast<int>(std::lround(dy * state->screen_h));
    if (shift_x == 0 && shift_y == 0) {
        return;
    }

    LockGuard guard(lock);
    // Move by whole pixels, so the already calculated pixels stay exactly aligned
    Fixed step(4.0 / state->zoom_factor / state->screen_w);
    state->center.real = clamp_coordinate(state->center.real + step.times(shift_x));
    state->center.imag = clamp_coordinate(state->center.imag + step.times(shift_y));
    state->shiftPixelState(-shift_x, -shift_y);
    first_limit_hint = estimate_first_limit();
    request_calculation();
}

void Fractalis::reset_view() {
    set_view({-0.5, 0}, 1.0);
}

void Fractalis::set_view(const Coordinate& center, double zoom) {
    LockGuard guard(lock);
    state->center = {clamp_coordinate(center.real), clamp_coordinate(center.imag)};
    state->zoom_factor = zoom;
    state->resetPixelComplete();
    first_limit_hint = 0;
    request_calculation();
}

void Fractalis::begin_supersampling() {
    ss_pass = true;
    state->supersampling = true;
    next_index = 0;
    order_p = 0;
    returned_count = 0;
    printf("Supersampling with %d samples per pixel\n", ss_samples);
}

void Fractalis::set_supersampling(int samples) {
    LockGuard guard(lock);
    ss_samples = samples == 2 || samples == 3 || samples == 4 || samples == 6 || samples == 8 ? samples : 1;
    // The pixels only keep the mean of their samples, so everything is calculated again. The image stays as preview.
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            state->pixelState[y][x].markIncomplete();
        }
    }
    first_limit_hint = estimate_first_limit();
    request_calculation();
}

void Fractalis::supersample() {
    LockGuard guard(lock);
    if (state->calculating != 0 || ss_samples < 2 || pass_id != state->calculation_id) {
        return;
    }
    begin_supersampling();
    state->calculating = 1;
}

void Fractalis::set_supersample_right_away(bool on) {
    LockGuard guard(lock);
    ss_right_away = on;
    // Like a change of the supersampling: everything again, the image stays as preview
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            state->pixelState[y][x].markIncomplete();
        }
    }
    first_limit_hint = estimate_first_limit();
    request_calculation();
}

void Fractalis::set_orbit_trap(int trap) {
    LockGuard guard(lock);
    trap_mode = trap >= TRAP_OFF && trap < TRAP_COUNT ? trap : TRAP_OFF;
    // Recalculate everything, the image stays as preview
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            state->pixelState[y][x].markIncomplete();
        }
    }
    first_limit_hint = 0;
    request_calculation();
}
