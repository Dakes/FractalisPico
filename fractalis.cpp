#include "fractalis.h"
#include "globals.h"
#include "palette.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace {

struct Escape {
    int iterations;
    float magnitude_sq;  // |z|^2 after escaping
    bool in_set;
    bool aborted;
};

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
template <typename T, typename Abort>
Escape iterate(T cr, T ci, int iter_limit, T epsilon, bool check_periodicity, const Abort& abort) {
    T zr = 0, zi = 0, zr2 = 0, zi2 = 0;
    T saved_r = 0, saved_i = 0;
    int save_at = 8;

    for (int n = 0; n < iter_limit; ++n) {
        zi = (zr + zr) * zi + ci;
        zr = zr2 - zi2 + cr;
        zr2 = zr * zr;
        zi2 = zi * zi;
        if (zr2 + zi2 > static_cast<T>(BAILOUT_SQ)) {
            return {n + 1, static_cast<float>(zr2 + zi2), false, false};
        }
        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
        if (check_periodicity) {
            if (std::abs(zr - saved_r) + std::abs(zi - saved_i) < epsilon) {
                return {iter_limit, 0.0f, true, false};
            }
            if (n == save_at) {
                saved_r = zr;
                saved_i = zi;
                save_at *= 2;
            }
        }
    }
    return {iter_limit, 0.0f, true, false};
}

// Lean double-double arithmetic for the inner loop. Accurate enough for iterating and skips the NaN/Inf handling
// of the DoubleDouble class.
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
    double nr_hi, nr_lo, ni_hi, ni_lo;  // nucleus
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
bool find_minibrot(DD cr, DD ci, int period, Minibrot& out, const Interrupt& interrupt) {
    const DD c0r = cr, c0i = ci;
    double last_step = INFINITY;
    bool converged = false;
    for (int step = 0; step < 12 && !converged; ++step) {
        if (interrupt()) return false;
        DD zr = {0, 0}, zi = {0, 0};
        double dr = 0, di = 0;  // dz/dc
        for (int i = 0; i < period; ++i) {
            double ndr = 2 * (zr.hi * dr - zi.hi * di) + 1;
            di = 2 * (zr.hi * di + zi.hi * dr);
            dr = ndr;
            DD zri = dd_mul(zr, zi);
            DD nzr = dd_add(dd_sub(dd_mul(zr, zr), dd_mul(zi, zi)), cr);
            zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
            zr = nzr;
            if (zr.hi * zr.hi + zi.hi * zi.hi > 4.0) return false;
        }
        double d = dr * dr + di * di;
        if (!(d > 0) || !std::isfinite(d)) return false;
        double sr = (zr.hi * dr + zi.hi * di) / d;
        double si = (zi.hi * dr - zr.hi * di) / d;
        cr = dd_sub(cr, {sr, 0});
        ci = dd_sub(ci, {si, 0});
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
        DD zr, zi;
        // first pass: the smallest |z_k|, second pass: the first k that is about as small
        for (int pass = 0; pass < 2; ++pass) {
            zr = zi = {0, 0};
            for (int k = 1; k <= period; ++k) {
                DD zri = dd_mul(zr, zi);
                DD nzr = dd_add(dd_sub(dd_mul(zr, zr), dd_mul(zi, zi)), cr);
                zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
                zr = nzr;
                double m = zr.hi * zr.hi + zi.hi * zi.hi;
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
    DD zr = {0, 0}, zi = {0, 0};
    for (int k = 0; k < actual; ++k) {
        complex z(zr.hi, zi.hi);
        a2 = 2.0 * (a * a + z * a2);
        a = 2.0 * z * a + 1.0;
        if (k >= 1) {
            dl = 2.0 * (l * l + z * dl);
            l = 2.0 * z * l;
            b += 1.0 / l;
        }
        DD zri = dd_mul(zr, zi);
        DD nzr = dd_add(dd_sub(dd_mul(zr, zr), dd_mul(zi, zi)), cr);
        zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
        zr = nzr;
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
    out.nr_hi = cr.hi; out.nr_lo = cr.lo;
    out.ni_hi = ci.hi; out.ni_lo = ci.lo;

    // Only useful if the reference itself is inside
    DD dcr = dd_sub(c0r, cr), dci = dd_sub(c0i, ci);
    float x = static_cast<float>(dcr.hi * out.scale_r - dci.hi * out.scale_i);
    float y = static_cast<float>(dcr.hi * out.scale_i + dci.hi * out.scale_r);
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
double interior_radius(DD zr, DD zi, DD cr, DD ci, int period, const Interrupt& interrupt) {
    using complex = std::complex<double>;
    double last_step = INFINITY;
    for (int step = 0; step < 10; ++step) {
        if (interrupt()) return 0;
        DD wr = zr, wi = zi;
        complex fz = 1, fc = 0, fzz = 0, fzc = 0;
        for (int i = 0; i < period; ++i) {
            complex w(wr.hi, wi.hi);
            fzz = 2.0 * (fz * fz + w * fzz);
            fzc = 2.0 * (fz * fc + w * fzc);
            fc = 2.0 * w * fc + 1.0;
            fz = 2.0 * w * fz;
            DD wri = dd_mul(wr, wi);
            DD nwr = dd_add(dd_sub(dd_mul(wr, wr), dd_mul(wi, wi)), cr);
            wi = dd_add({wri.hi * 2, wri.lo * 2}, ci);
            wr = nwr;
            if (wr.hi * wr.hi + wi.hi * wi.hi > 4.0) return 0;
        }
        if (!(std::abs(fz) < 1.0)) return 0;
        complex g = complex(dd_sub(wr, zr).hi, dd_sub(wi, zi).hi);
        complex delta = g / (fz - 1.0);
        double step_size = std::abs(delta);
        // Converged (as far as the precision goes): the derivatives at z are accurate enough
        if (step_size < 1e-12 || (step_size > last_step * 0.5 && step_size < 1e-6)) {
            double b = (1.0 - std::norm(fz)) / std::abs(fzc + fzz * fc / (1.0 - fz));
            return std::isfinite(b) ? 0.9 * b / 4.0 : 0;
        }
        last_step = step_size;
        zr = dd_sub(zr, {delta.real(), 0});
        zi = dd_sub(zi, {delta.imag(), 0});
    }
    return 0;
}

template <typename Abort>
Escape iterate_dd(DD cr, DD ci, int iter_limit, const Abort& abort) {
    DD zr = {0, 0}, zi = {0, 0}, zr2 = {0, 0}, zi2 = {0, 0};

    for (int n = 0; n < iter_limit; ++n) {
        DD zri = dd_mul(zr, zi);
        zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
        zr = dd_add(dd_sub(zr2, zi2), cr);
        zr2 = dd_mul(zr, zr);
        zi2 = dd_mul(zi, zi);
        double magnitude_sq = zr2.hi + zi2.hi;
        if (magnitude_sq > BAILOUT_SQ) {
            return {n + 1, static_cast<float>(magnitude_sq), false, false};
        }
        if ((n & ABORT_CHECK_MASK) == ABORT_CHECK_MASK && abort()) {
            return {n, 0.0f, false, true};
        }
    }
    return {iter_limit, 0.0f, true, false};
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
template <typename Abort>
Escape iterate_perturbed(const float* orbit, int orbit_length, float dcr, float dci, int iter_limit, float epsilon,
                         const Abort& abort) {
    float dzr = 0, dzi = 0;
    int m = 0;
    float saved_zr = NAN, saved_zi = NAN, saved_dzr = 0, saved_dzi = 0;
    int save_at = 8;
    const float max_compared_dz = epsilon * 1e6f;
    for (int n = 0; n < iter_limit; ++n) {
        float ar = 2.0f * orbit[2 * m] + dzr;
        float ai = 2.0f * orbit[2 * m + 1] + dzi;
        float nr = ar * dzr - ai * dzi + dcr;
        dzi = ar * dzi + ai * dzr + dci;
        dzr = nr;
        m++;
        float zr = orbit[2 * m] + dzr;
        float zi = orbit[2 * m + 1] + dzi;
        float magnitude_sq = zr * zr + zi * zi;
        if (magnitude_sq > static_cast<float>(BAILOUT_SQ)) {
            return {n + 1, magnitude_sq, false, false};
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
            return {iter_limit, 0.0f, true, false};
        }
        if (n == save_at) {
            saved_zr = ref_r;
            saved_zi = ref_i;
            saved_dzr = dzr;
            saved_dzi = dzi;
            save_at *= 2;
        }
    }
    return {iter_limit, 0.0f, true, false};
}

}  // namespace

Fractalis::Fractalis(FractalisState* state)
    : state(state), pass_id(0), pass_limit(0), pass_target(0), pass_resolved(0), next_index(0), in_flight(0),
      returned_count(0), first_limit_hint(0) {
    ref = {};
    // Z_0 .. Z_MAX_ITER
    ref.orbit = new float[2 * (MAX_ITER + 1)];
}

Fractalis::~Fractalis() {
    delete[] ref.orbit;
}

int Fractalis::max_iterations(double zoom) const {
    double scale = state->screen_w / (3.0 / zoom);
    int max_iter = static_cast<int>(50 * std::pow(std::log10(scale), 1.25));
    return std::min(max_iter, MAX_ITER);
}

bool Fractalis::calculate_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                                PixelState& pixel) const {
    auto abort = [this, id, interrupt]() {
        return state->calculation_id != id || (interrupt && interrupt());
    };
    Escape escape;

    if (view.zoom < FLOAT_MAX_ZOOM) {
        // Everything in single precision, double math is slower and would cost more than the iterations
        float cr = view.center_rf + ((x + 0.5f - state->screen_w / 2.0f) * view.step_f + view.center_rf_low);
        float ci = view.center_if + ((y + 0.5f - state->screen_h / 2.0f) * view.step_f + view.center_if_low);
        if (is_in_main_bulb(cr, ci)) {
            escape = {iter_limit, 0.0f, true, false};
        } else {
            escape = iterate<float>(cr, ci, iter_limit, view.step_f * 1e-3f, true, abort);
        }
    } else {
        // Offset from the center in the complex plane. Exact enough in double, even for deep zooms.
        double offset_r = (x + 0.5 - state->screen_w / 2.0) * view.step;
        double offset_i = (y + 0.5 - state->screen_h / 2.0) * view.step;
        double cr = view.center.real.upper + offset_r;
        double ci = view.center.imag.upper + offset_i;
        bool optimize = view.zoom < OPTIMIZATIONS_MAX_ZOOM;

        if (optimize && is_in_main_bulb(cr, ci)) {
            escape = {iter_limit, 0.0f, true, false};
        } else if (view.perturbed && (view.ref_offset_r + offset_r) * (view.ref_offset_r + offset_r)
                   + (view.ref_offset_i + offset_i) * (view.ref_offset_i + offset_i) < view.interior_radius_sq) {
            escape = {iter_limit, 0.0f, true, false};
        } else if (view.perturbed && view.minibrot && is_in_component(view.cardioid,
                       static_cast<float>((view.nucleus_offset_r + offset_r) * view.scale_r
                                          - (view.nucleus_offset_i + offset_i) * view.scale_i),
                       static_cast<float>((view.nucleus_offset_r + offset_r) * view.scale_i
                                          + (view.nucleus_offset_i + offset_i) * view.scale_r))) {
            escape = {iter_limit, 0.0f, true, false};
        } else if (view.perturbed) {
            float dcr = static_cast<float>(view.ref_offset_r + offset_r);
            float dci = static_cast<float>(view.ref_offset_i + offset_i);
            escape = iterate_perturbed(view.orbit, view.orbit_length, dcr, dci, iter_limit,
                                       static_cast<float>(view.step * 1e-3), abort);
        } else if (view.zoom < DOUBLE_MAX_ZOOM) {
            escape = iterate<double>(cr, ci, iter_limit, view.step * 1e-3, optimize, abort);
        } else {
            DoubleDouble re = view.center.real + offset_r;
            DoubleDouble im = view.center.imag + offset_i;
            escape = iterate_dd({re.upper, re.lower}, {im.upper, im.lower}, iter_limit, abort);
        }
    }

    if (escape.aborted) {
        return false;
    }

    pixel.clear();
    pixel.flags = PixelState::COMPLETE | PixelState::VALID;
    if (escape.in_set) {
        pixel.flags |= PixelState::IN_SET;
    } else {
        // Smooth (continuous) iteration count. The +2 (instead of the usual +1) keeps the palette as it was.
        float log2_z = 0.5f * std::log2(escape.magnitude_sq);
        float smooth = escape.iterations + 2 - std::log2(log2_z);
        pixel.setPosition(palette::position(smooth));
    }
    return true;
}

bool Fractalis::claim_pixel(int& x, int& y) {
    // Pixels an interrupted core gave back come first
    while (returned_count > 0) {
        returned_count--;
        x = returned[returned_count].x;
        y = returned[returned_count].y;
        if (!state->pixelState[y][x].isComplete()) {
            return true;
        }
    }

    // Pixels are handed out in square rings around the screen center.
    // Ring r > 0 starts at index (2r - 1)^2 and has 4 sides with 2r pixels each.
    const int cx = state->screen_w / 2;
    const int cy = state->screen_h / 2;
    const int max_radius = std::max(std::max(cx, state->screen_w - cx), std::max(cy, state->screen_h - cy));
    const int total = (2 * max_radius + 1) * (2 * max_radius + 1);

    while (next_index < total) {
        int index = next_index++;
        if (index == 0) {
            x = cx;
            y = cy;
        } else {
            int r = (static_cast<int>(std::sqrt(static_cast<float>(index))) + 1) / 2;
            // fix possible rounding errors of the float sqrt
            while ((2 * r + 1) * (2 * r + 1) <= index) r++;
            while ((2 * r - 1) * (2 * r - 1) > index) r--;
            int side_length = 2 * r;
            int offset = index - (2 * r - 1) * (2 * r - 1);
            int side = offset / side_length;
            int pos = offset % side_length;
            switch (side) {
                case 0: x = cx - r + pos; y = cy - r; break;  // top, left to right
                case 1: x = cx + r; y = cy - r + pos; break;  // right, top to bottom
                case 2: x = cx + r - pos; y = cy + r; break;  // bottom, right to left
                default: x = cx - r; y = cy + r - pos; break; // left, bottom to top
            }
            bool horizontal = side == 0 || side == 2;
            if (horizontal && (y < 0 || y >= state->screen_h)) {
                // skip the rest of this side
                next_index = index - pos + side_length;
                continue;
            }
        }
        if (x < 0 || x >= state->screen_w || y < 0 || y >= state->screen_h) {
            continue;
        }
        if (!state->pixelState[y][x].isComplete()) {
            return true;
        }
    }
    return false;
}

void Fractalis::start_pass() {
    // A new view: start with a low iteration limit, so a first image appears quickly
    pass_id = state->calculation_id;
    pass_view.center = state->center;
    pass_view.zoom = state->zoom_factor;
    pass_view.step = 4.0 / state->zoom_factor / state->screen_w;
    pass_view.center_rf = static_cast<float>(pass_view.center.real.upper);
    pass_view.center_if = static_cast<float>(pass_view.center.imag.upper);
    pass_view.center_rf_low = static_cast<float>((pass_view.center.real - pass_view.center_rf).upper);
    pass_view.center_if_low = static_cast<float>((pass_view.center.imag - pass_view.center_if).upper);
    pass_view.step_f = static_cast<float>(pass_view.step);
    pass_view.perturbed = pass_view.zoom >= PERTURBATION_MIN_ZOOM && pass_view.zoom < DOUBLE_DOUBLE_MAX_ZOOM;

    returned_count = 0;

    // Keep the reference orbit while C is close to the screen. Further away, the pixel distances get lost in the
    // single precision dc.
    bool keep_reference = ref.length > 0 || ref.busy;
    if (keep_reference) {
        double dx = (ref.c.real - pass_view.center.real).upper / pass_view.step;
        double dy = (ref.c.imag - pass_view.center.imag).upper / pass_view.step;
        keep_reference = std::abs(dx) < state->screen_w && std::abs(dy) < state->screen_w;
    }

    int target = max_iterations(state->zoom_factor);
    int first;
    if (first_limit_hint > 0) {
        // Most pixels escape around the hint, lower passes would only show the preview again
        first = std::max(FIRST_PASS_ITER, std::min(first_limit_hint, target));
        if (first * 3 / 2 >= target) first = target;
    } else {
        first = next_pass_limit(std::max(FIRST_PASS_ITER, target / 16) / 2, target);
    }
    begin_pass(first, target);
    // (begin_pass already chose a new one, if the kept orbit is too short)
    if (pass_view.perturbed && !keep_reference && ref.tries == 0) {
        choose_reference();
    }
}

int Fractalis::estimate_first_limit() const {
    // Median of the smooth iteration counts on screen, from a histogram of the palette positions
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
    for (int sum = 0; bin < BINS - 1 && (sum += histogram[bin]) < count / 2; ++bin) {}
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
    next_index = 0;
    state->iteration_limit = limit;
    if (pass_view.perturbed) {
        ref.tries = 0;
        ref.best_length = 0;
        if (ref.escaped && ref.length <= limit) {
            // The reference escapes too early for this pass, take one of the pixels that are still undecided
            choose_reference();
        }
    }
    printf("Starting pass %lu with iteration limit %d\n", static_cast<unsigned long>(pass_id), limit);
}

void Fractalis::finish_pass() {
    // Count the pixels that are still undecided
    int unresolved = 0;
    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            if (state->pixelState[y][x].isInSet()) unresolved++;
        }
    }

    int next_limit = 0;
    if (unresolved > 0) {
        if (pass_limit < pass_target) {
            next_limit = next_pass_limit(pass_limit, pass_target);
        } else {
            // Keep refining while the higher limits still reveal new details
            int max_target = std::min(MAX_ITER, max_iterations(pass_view.zoom) * REFINE_MAX_FACTOR);
            if (!state->auto_zoom && pass_limit < max_target && pass_resolved >= REFINE_MIN_PIXELS) {
                pass_target = std::min(pass_limit * 2, max_target);
                next_limit = pass_target;
            }
        }
    }

    for (int y = 0; y < state->screen_h; ++y) {
        for (int x = 0; x < state->screen_w; ++x) {
            PixelState& pixel = state->pixelState[y][x];
            if (!pixel.isInSet()) continue;
            if (next_limit) {
                // Pixels that escaped keep their value with a higher limit, only these need another pass
                pixel.markIncomplete();
            } else {
                // Final: in the set, no more preview colors
                pixel.flags &= ~PixelState::PREVIEW_COLOR;
            }
        }
    }

    state->completed_limit = static_cast<uint16_t>(pass_limit);
    if (next_limit) {
        begin_pass(next_limit, pass_target);
    } else {
        state->calculating = 0;
        printf("Calculation complete at iteration limit %d\n", pass_limit);
    }
    state->passes_completed++;
    state->needs_redraw = true;
}

void Fractalis::store_result(int x, int y, const PixelState& result) {
    PixelState& target = state->pixelState[y][x];
    if (result.isInSet() && target.showsColor()) {
        // Undecided at this iteration limit: keep showing the zoom preview instead of black for now
        target.flags = PixelState::COMPLETE | PixelState::VALID | PixelState::IN_SET | PixelState::PREVIEW_COLOR;
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
    // The pixel center, exactly like calculate_pixel()
    Coordinate c = pass_view.center;
    c.real += (best_x + 0.5 - state->screen_w / 2.0) * pass_view.step;
    c.imag += (best_y + 0.5 - state->screen_h / 2.0) * pass_view.step;
    set_reference(c);
}

bool Fractalis::extend_reference(uint32_t id, int target_length, bool (*interrupt)()) {
    uint32_t generation;
    DD cr, ci, zr, zi;
    int n;
    {
        LockGuard guard(lock);
        generation = ref.generation;
        cr = {ref.c.real.upper, ref.c.real.lower};
        ci = {ref.c.imag.upper, ref.c.imag.lower};
        zr = {ref.zr.upper, ref.zr.lower};
        zi = {ref.zi.upper, ref.zi.lower};
        n = ref.length;
    }

    // Nobody reads the orbit while it is not ready
    float* orbit = ref.orbit;
    if (n == 0) {
        orbit[0] = orbit[1] = 0.0f;
        zr = zi = {0, 0};
        n = 1;
    }
    bool escaped = false;
    bool aborted = false;
    while (n < target_length) {
        DD zr2 = dd_mul(zr, zr);
        DD zi2 = dd_mul(zi, zi);
        DD zri = dd_mul(zr, zi);
        zi = dd_add({zri.hi * 2, zri.lo * 2}, ci);
        zr = dd_add(dd_sub(zr2, zi2), cr);
        orbit[2 * n] = static_cast<float>(zr.hi);
        orbit[2 * n + 1] = static_cast<float>(zi.hi);
        n++;
        if (zr.hi * zr.hi + zi.hi * zi.hi > BAILOUT_SQ) {
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
        if (ref.generation != generation) {
            return true;  // a new C was chosen in the mean time
        }
        // Also an aborted orbit is valid as far as it got
        ref.length = n;
        ref.escaped = escaped;
        ref.zr = DoubleDouble(zr.hi, zr.lo);
        ref.zi = DoubleDouble(zi.hi, zi.lo);

        if (escaped && n <= pass_limit && id == pass_id && pass_view.perturbed) {
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

    // Takes a few thousand double-double iterations, the orbit is already usable in the mean time
    bool interrupted = false;
    auto check_interrupt = [&]() {
        return interrupted = interrupt && interrupt();
    };
    Minibrot minibrot;
    bool found = find_minibrot(cr, ci, period_guess, minibrot, check_interrupt);
    // The last orbit value is close to the attracting cycle
    double radius = found && !interrupted ? interior_radius(zr, zi, cr, ci, minibrot.period, check_interrupt) : 0;

    LockGuard guard(lock);
    if (ref.generation == generation) {
        if (interrupted) {
            ref.minibrot = Reference::MINIBROT_UNKNOWN;  // try again
        } else if (found && (minibrot.usable || radius > 0)) {
            ref.minibrot = Reference::MINIBROT_FOUND;
            ref.component_check = minibrot.usable;
            ref.period = minibrot.period;
            ref.nucleus = {DoubleDouble(minibrot.nr_hi, minibrot.nr_lo), DoubleDouble(minibrot.ni_hi, minibrot.ni_lo)};
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
    int count = 0;
    uint32_t id;
    View view;
    int iter_limit;
    bool calculate_reference = false;
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
        while (count < batch && claim_pixel(xs[count], ys[count])) {
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
            view.ref_offset_r = (view.center.real - ref.c.real).upper;
            view.ref_offset_i = (view.center.imag - ref.c.imag).upper;
            view.orbit = ref.orbit;
            view.orbit_length = ref.length;
            view.minibrot = ref.minibrot == Reference::MINIBROT_FOUND && ref.component_check;
            view.interior_radius_sq = ref.minibrot == Reference::MINIBROT_FOUND
                                    ? ref.interior_radius * ref.interior_radius : 0;
            if (view.minibrot) {
                view.nucleus_offset_r = (view.center.real - ref.nucleus.real).upper;
                view.nucleus_offset_i = (view.center.imag - ref.nucleus.imag).upper;
                view.cardioid = ref.cardioid;
                view.scale_r = ref.scale_r;
                view.scale_i = ref.scale_i;
            }
        }
        in_flight++;
    }

    int done = 0;
    while (done < count && calculate_pixel(xs[done], ys[done], view, iter_limit, id, interrupt, results[done])) {
        done++;
    }

    LockGuard guard(lock);
    in_flight--;
    // Throw the results away, if the view changed in the mean time
    if (id != state->calculation_id) {
        return true;
    }
    for (int i = 0; i < done; ++i) {
        store_result(xs[i], ys[i], results[i]);
    }
    // Interrupted: give the rest back, so they are calculated by the next free core
    for (int i = done; i < count && returned_count < RETURNED_CAPACITY; ++i) {
        returned[returned_count++] = {static_cast<int16_t>(xs[i]), static_cast<int16_t>(ys[i])};
    }
    return done == count;
}

void Fractalis::request_calculation() {
    state->completed_limit = 0;
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
    double step = 4.0 / state->zoom_factor / state->screen_w;
    state->center.real += DoubleDouble(shift_x) * step;
    state->center.imag += DoubleDouble(shift_y) * step;
    state->shiftPixelState(-shift_x, -shift_y);
    first_limit_hint = estimate_first_limit();
    request_calculation();
}

void Fractalis::reset_view() {
    LockGuard guard(lock);
    state->center = {-0.5, 0};
    state->zoom_factor = 1.0;
    state->resetPixelComplete();
    first_limit_hint = 0;
    request_calculation();
}
