#include "fractalis.h"
#include "globals.h"
#include "palette.h"
#include <algorithm>
#include <cmath>
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

}  // namespace

Fractalis::Fractalis(FractalisState* state)
    : state(state), pass_id(0), pass_limit(0), pass_target(0), pass_resolved(0), next_index(0), in_flight(0),
      returned_count(0), first_limit_hint(0) {}

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
        // Everything in single precision, double math is software emulated and would cost more than the iterations
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

    returned_count = 0;

    int target = max_iterations(state->zoom_factor);
    int first;
    if (state->auto_zoom) {
        // Auto zoom doesn't need to watch the details appear, it goes straight for the full limit
        first = target;
    } else if (first_limit_hint > 0) {
        // Most pixels escape around the hint, lower passes would only show the preview again
        first = std::max(FIRST_PASS_ITER, std::min(first_limit_hint, target));
        if (first * 3 / 2 >= target) first = target;
    } else {
        first = next_pass_limit(std::max(FIRST_PASS_ITER, target / 16) / 2, target);
    }
    begin_pass(first, target);
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
    {
        LockGuard guard(lock);
        if (state->calculating == 0) {
            return false;
        }
        if (pass_id != state->calculation_id) {
            start_pass();
        }
        int batch = pass_view.zoom < FLOAT_MAX_ZOOM ? MAX_BATCH : pass_view.zoom < DOUBLE_MAX_ZOOM ? 4 : 1;
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
