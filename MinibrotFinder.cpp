#include "MinibrotFinder.hpp"
#include "globals.h"
#include <algorithm>

namespace {

using complex = std::complex<double>;

// Around a minibrot, the corners of the screen escape after about 7 periods when it fills the screen, its colorful
// surroundings take a lot more. With a higher period, the iteration limit isn't enough for a picture.
constexpr int PERIOD_LIMIT = MAX_ITER / 16;
// Points that haven't escaped by then count as in the set
constexpr int ESCAPE_LIMIT = MAX_ITER;
constexpr int MAX_WALK_STEPS = 400;
constexpr int MAX_NEWTON_STEPS = 40;
constexpr int MAX_PROBES_PER_START = 8;
// The whole search, about 10 s
constexpr uint32_t MAX_ITERATIONS = 1'200'000;
// A single probe
constexpr uint32_t MAX_PROBE_ITERATIONS = 100'000;
// The biggest minibrot in a disc of radius r on the edge of the set is about r^2 / 16 big
constexpr double SIZE_PER_RADIUS_SQ = 1.0 / 16;
// The walk gets this much closer to the set than the radius of the disc
constexpr double WALK_CLOSER = 8;
// Off by less than this many powers of ten of zoom: good enough
constexpr double CLOSE_ENOUGH = 0.3;
// Off by more: worth trying the next start point
constexpr double TRY_NEXT_START = 1.0;

Coordinate offset(const Coordinate& c, complex d) {
    return {c.real + Fixed(d.real()), c.imag + Fixed(d.imag())};
}

complex difference(const Coordinate& a, const Coordinate& b) {
    return {(a.real - b.real).to_double(), (a.imag - b.imag).to_double()};
}

}  // namespace

void MinibrotFinder::Orbit::begin(const Coordinate& c) {
    cr = c.real;
    ci = c.imag;
    zr = 0.0;
    zi = 0.0;
    z = dz = 0.0;
    n = 0;
}

void MinibrotFinder::Orbit::step() {
    dz = 2.0 * z * dz + 1.0;
    Fixed zri = zr * zi;
    zr = zr * zr - zi * zi + cr;
    zi = zri.twice() + ci;
    z = complex(zr.to_double(), zi.to_double());
    n++;
}

void MinibrotFinder::start(const Coordinate* points, int count, double target) {
    start_count = std::min(count, MAX_STARTS);
    for (int i = 0; i < start_count; ++i) starts[i] = points[i];
    target_zoom = target;
    single = false;
    max_iterations = MAX_ITERATIONS;
    best_error = INFINITY;
    total_iterations = 0;
    probe_count = 0;
    start_index = -1;
    next_start();
}

void MinibrotFinder::probe(const Coordinate& c, double r) {
    single = true;
    max_iterations = MAX_PROBE_ITERATIONS;
    best_error = INFINITY;
    total_iterations = 0;
    probe_count = 1;
    point = c;
    radius = r;
    orbit.begin(point);
    phase = BALL;
}

void MinibrotFinder::next_start() {
    if (++start_index >= start_count) {
        phase = IDLE;
        return;
    }
    have_point = false;
    distance = INFINITY;
    walk_factor = 0.5;
    walk_steps = 0;
    radius = std::sqrt(1.0 / (target_zoom * SIZE_PER_RADIUS_SQ));
    walk_goal = radius / WALK_CLOSER;
    deep_radius = 0;
    shallow_radius = INFINITY;
    start_probes = 0;
    trial = starts[start_index];
    orbit.begin(trial);
    phase = WALK;
}

bool MinibrotFinder::work(int iterations) {
    for (int i = 0; i < iterations && phase != IDLE; ++i) {
        if (++total_iterations > max_iterations) {
            phase = IDLE;
            break;
        }
        const complex z = orbit.z;  // before the step, for the size estimate
        orbit.step();
        switch (phase) {
            case WALK:
                if (std::norm(orbit.z) > 16) {
                    escaped();
                } else if (orbit.n >= ESCAPE_LIMIT) {
                    // In the set, or too close to it: a smaller step
                    if (!have_point) {
                        next_start();
                    } else if ((walk_factor *= 0.5) < 1.0 / 64) {
                        begin_probe();
                    } else {
                        step_towards_set();
                    }
                }
                break;

            case BALL: {
                const double m = std::norm(orbit.z);
                if (m < radius * radius * std::norm(orbit.dz)) {
                    // Newton's method starts with this orbit
                    period = orbit.n;
                    newton_c = point;
                    newton_steps = 0;
                    newton_step(false);
                } else if (m > 4 || orbit.n >= PERIOD_LIMIT) {
                    // No minibrot in the disc with a period below the limit: they are all smaller
                    deep_radius = std::max(deep_radius, radius);
                    next_radius(radius * 4);
                }
                break;
            }

            case NEWTON:
                if (std::norm(orbit.z) > 16) {
                    // The step went too far, out of the set: a shorter one
                    if ((newton_damping *= 0.5) < 1.0 / 64) {
                        next_radius(radius * 0.4);
                    } else {
                        newton_c = offset(newton_base, -newton_change * newton_damping);
                        newton_delta = std::abs(newton_change) * newton_damping;
                        begin_newton_orbit();
                    }
                    break;
                }
                a2 = 2.0 * (a * a + z * a2);
                a = 2.0 * z * a + 1.0;
                if (orbit.n >= 2) {
                    dl = 2.0 * (l * l + z * dl);
                    l = 2.0 * z * l;
                    b += 1.0 / l;
                }
                if (orbit.n < period && period % orbit.n == 0) {
                    divisor_min = std::min(divisor_min, std::norm(orbit.z));
                }
                if (orbit.n == period) newton_step(true);
                break;

            case IDLE:
                break;
        }
    }
    return phase != IDLE;
}

void MinibrotFinder::begin_newton_orbit() {
    l = 1.0;
    dl = 0.0;
    b = 1.0;
    a = 0.0;
    a2 = 0.0;
    divisor_min = INFINITY;
    orbit.begin(newton_c);
    phase = NEWTON;
}

/**
 * Newton's method for z_period(c) = 0, the orbit of newton_c is done up to the period. estimated: the size estimate
 * was calculated along with it.
 */
void MinibrotFinder::newton_step(bool estimated) {
    const complex delta = orbit.z / orbit.dz;
    if (!std::isfinite(delta.real()) || !std::isfinite(delta.imag()) || std::abs(delta) > 8 * radius) {
        next_radius(radius * 0.4);
        return;
    }
    // 1 / size
    const double zoom = estimated ? std::abs(b * l * l) : 0;
    const double last_delta = newton_steps > 0 ? newton_delta : INFINITY;
    newton_base = newton_c;
    newton_change = delta;
    newton_damping = 1;
    newton_c = offset(newton_c, -delta);
    newton_delta = std::abs(delta);
    // Until the step is tiny compared to the size, or doesn't get smaller any more at the precision limit
    const double relative = newton_delta * zoom;
    if (estimated && (relative < 1e-6 || (relative < 1e-2 && newton_delta > last_delta * 0.5))) {
        finish_probe(zoom);
    } else if (++newton_steps >= MAX_NEWTON_STEPS) {
        next_radius(radius * 0.4);
    } else {
        begin_newton_orbit();
    }
}

// Escaped: the distance estimate, and the next step towards the set
void MinibrotFinder::escaped() {
    // The rest in double, it doesn't need the precision any more
    complex z = orbit.z, dz = orbit.dz;
    const complex c(trial.real.to_double(), trial.imag.to_double());
    for (int i = 0; i < 64 && std::norm(z) < 1e20; ++i) {
        dz = 2.0 * z * dz + 1.0;
        z = z * z + c;
    }
    const double r = std::abs(z);
    const double estimate = 2 * r * std::log(r) / std::abs(dz);
    // Steepest ascent of the potential log|z_n|
    const complex gradient = std::conj(dz / z);
    if (!(estimate > 0) || !std::isfinite(estimate) || !(std::abs(gradient) > 0)) {
        if (!have_point) {
            next_start();
        } else {
            begin_probe();
        }
        return;
    }
    point = trial;
    have_point = true;
    distance = estimate;
    away = gradient / std::abs(gradient);
    walk_steps++;
    walk_factor = std::min(0.5, walk_factor * 2);
    if (distance < walk_goal || walk_steps >= MAX_WALK_STEPS) {
        begin_probe();
    } else {
        step_towards_set();
    }
}

// The true distance is at least a quarter of the estimate, half of it is mostly safe
void MinibrotFinder::step_towards_set() {
    trial = offset(point, -away * (distance * walk_factor));
    orbit.begin(trial);
    phase = WALK;
}

void MinibrotFinder::begin_probe() {
    if (start_probes >= MAX_PROBES_PER_START) {
        end_start();
        return;
    }
    // The point has to be a lot closer to the set than the radius
    if (distance > radius / WALK_CLOSER && walk_steps < MAX_WALK_STEPS && walk_factor >= 1.0 / 64) {
        walk_goal = radius / WALK_CLOSER;
        step_towards_set();
        return;
    }
    start_probes++;
    probe_count++;
    orbit.begin(point);
    phase = BALL;
}

// Newton's method has converged, the size estimate and shape are from the orbit before the last (tiny) step
void MinibrotFinder::finish_probe(double zoom) {
    // A minibrot is a cardioid, the bulbs attached to it are discs (see find_minibrot() in fractalis.cpp)
    const complex e = 0.5 + (l * (a2 * 0.5) + dl * a) / (2.0 * l * l * a * a);
    const bool valid = std::isfinite(zoom) && zoom > 0 && std::abs(e - 0.5) < 0.1
                    && !(divisor_min <= std::norm(orbit.z) * 1e6)  // the nucleus of a lower period
                    && std::abs(difference(newton_c, point)) < 8 * radius;
    if (!valid) {
        // Mostly works with another period
        next_radius(radius * 0.4);
        return;
    }
    if (single) {
        best = {newton_c, period, b * l * l};
        best_error = 0;
        phase = IDLE;
        return;
    }
    const double error = std::log10(zoom / target_zoom);
    if (zoom <= PRECISION_MAX_ZOOM && std::abs(error) < best_error) {
        best = {newton_c, period, b * l * l};
        best_error = std::abs(error);
    }
    if (best_error < CLOSE_ENOUGH) {
        phase = IDLE;
        return;
    }
    if (zoom > target_zoom) {
        deep_radius = std::max(deep_radius, radius);
    } else {
        shallow_radius = std::min(shallow_radius, radius);
    }
    // The size goes with the square of the radius
    next_radius(radius * std::pow(10.0, error / 2));
}

void MinibrotFinder::next_radius(double proposed) {
    if (single) {
        phase = IDLE;
        return;
    }
    // A bigger disc has a bigger minibrot. Between a radius that was too small and one that was too big, the next
    // one is in the middle (log scale), once the guess doesn't help.
    if (deep_radius > 0 && shallow_radius < INFINITY) {
        if (shallow_radius < deep_radius * 1.5) {
            end_start();
            return;
        }
        if (proposed <= deep_radius * 1.1 || proposed >= shallow_radius / 1.1) {
            proposed = std::sqrt(deep_radius * shallow_radius);
        }
    } else if (proposed <= deep_radius) {
        proposed = deep_radius * 2;
    } else if (proposed >= shallow_radius) {
        proposed = shallow_radius / 2;
    }
    radius = proposed;
    begin_probe();
}

void MinibrotFinder::end_start() {
    if (best_error < TRY_NEXT_START) {
        phase = IDLE;
    } else {
        next_start();
    }
}
