#include "BulbNamer.hpp"
#include <algorithm>
#include <cmath>

namespace {

using complex = std::complex<double>;

constexpr uint32_t MAX_ITERATIONS = 1'500'000;
// Longest cycle it looks for
constexpr int MAX_SEARCH = 4096;
// Closer than this, the orbit counts as back (the cycle length the orbit viewer shows)
constexpr double CLOSE = 1e-3;
constexpr int MAX_NEWTON_STEPS = 40;
constexpr int MAX_ROOT_STEPS = 16;
constexpr int PATH_STEPS = 16;
// Not quite 1: at the root of a bulb two solutions meet, Newton's method gets slow there
constexpr double ROOT_MULTIPLIER = 0.999;
// |mu^k - 1| of the parent's cycle there: about (1 - ROOT_MULTIPLIER) / 2 for k = 2
constexpr double PARENT_TOLERANCE = 0.02;
// Newton's method has converged when the residual is this much smaller than at the start, or tiny in absolute terms
// (the orbit was converged to the precision limit already)
constexpr double CONVERGED = 1e-12;
constexpr double TINY = 1e-60;

void add(Fixed& re, Fixed& im, const complex& d) {
    re += Fixed(d.real());
    im += Fixed(d.imag());
}

int gcd(int a, int b) {
    while (b) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

bool finite(const complex& z) {
    return std::isfinite(z.real()) && std::isfinite(z.imag());
}

}  // namespace

void BulbNamer::start(const Coordinate& c, const Fixed& z_re, const Fixed& z_im, int hint) {
    cr = c.real;
    ci = c.imag;
    start_zr = zr = z_re;
    start_zi = zi = z_im;
    total = 0;
    period_ = depth_ = base = 0;
    named_ = false;
    candidate_count = 0;
    if (hint > 0) add_candidate(hint);
    best_distance = INFINITY;
    best_k = first_close = 0;
    begin(MAX_SEARCH, false);
    phase = SEARCH;
}

void BulbNamer::begin(int steps, bool with_derivatives) {
    er = zr;
    ei = zi;
    n = 0;
    length = steps;
    derivatives = with_derivatives;
    A = 1.0;
    B = C = D = 0.0;
}

bool BulbNamer::step() {
    if (derivatives) {
        const complex z(er.to_double(), ei.to_double());
        const complex a = A, b = B;
        C = 2.0 * (a * a + z * C);
        D = 2.0 * (a * b + z * D);
        A = 2.0 * z * a;
        B = 2.0 * z * b + 1.0;
    }
    const Fixed zri = er * ei;
    er = er * er - ei * ei + cr;
    ei = zri.twice() + ci;
    n++;
    // Cycles stay within |z| <= 2
    const double re = er.to_double(), im = ei.to_double();
    return re * re + im * im < 16.0;
}

BulbNamer::complex BulbNamer::distance() const {
    return {(er - zr).to_double(), (ei - zi).to_double()};
}

bool BulbNamer::work(int iterations) {
    for (int i = 0; i < iterations && phase != IDLE; ++i) {
        if (++total > MAX_ITERATIONS) {
            phase = IDLE;
            break;
        }
        if (!step()) {
            // Escaped: no cycle there
            switch (phase) {
                case SEARCH:
                case SEARCH_AGAIN: phase = IDLE; break;
                case PERIOD:
                case SCAN: next_candidate(); break;
                case CENTROID:
                case PARENT: next_divisor(); break;
                default: phase = IDLE; break;
            }
            continue;
        }
        switch (phase) {
            case SEARCH: {
                // How close the orbit comes back to where it was, after k = n steps
                const double d = std::abs(distance());
                if (d < best_distance) {
                    best_distance = d;
                    best_k = n;
                }
                if (!first_close && d < CLOSE) first_close = n;
                if (n == length) {
                    if (first_close) add_candidate(first_close);
                    add_candidate(best_k);
                    begin(MAX_SEARCH, false);
                    phase = SEARCH_AGAIN;
                }
                break;
            }
            case SEARCH_AGAIN:
                // The first time it comes back about as close as it ever does: mostly the cycle itself, the best one
                // can be a multiple of it
                if (std::abs(distance()) <= 4 * best_distance) {
                    add_candidate(n);
                    try_candidates();
                } else if (n == length) {
                    try_candidates();
                }
                break;
            case PERIOD:
                if (n == length) {
                    bool done;
                    if (newton_z(done) && done) {
                        if (std::abs(A) < 1.0) {
                            // Attracting. The shortest cycle in it is the period.
                            scan_tolerance = std::max(1e3 * std::abs(distance()), 1e-300);
                            begin(length, true);
                            phase = SCAN;
                        } else {
                            next_candidate();
                        }
                    } else if (!done) {
                        begin(length, true);
                    } else {
                        next_candidate();
                    }
                }
                break;
            case SCAN:
                if (std::abs(distance()) <= scan_tolerance || n == length) found_period(n, A);
                break;
            case ROOT:
                if (n == length) root_step();
                break;
            case CENTROID:
                // The k points of the bulb's cycle around a point of the parent's cycle: z_0, z_q, z_2q, ...
                if (n % q == 0) sum += complex((er - root_zr).to_double(), (ei - root_zi).to_double());
                if (n == length) {
                    zr = root_zr;
                    zi = root_zi;
                    add(zr, zi, sum / static_cast<double>(k_try));
                    newton_steps = 0;
                    last_step = INFINITY;
                    begin(q, true);
                    phase = PARENT;
                }
                break;
            case PARENT:
                if (n == length) {
                    bool done;
                    if (newton_z(done) && done) {
                        /**
                         * On the parent's boundary at the angle m/k: mu^k = 1. The bulb's multiplier isn't quite 1 at
                         * this point, mu^k is off by about as much. Any other cycle is off by a lot more, the power
                         * blows up the difference.
                         */
                        const complex mu = A;
                        double turns = std::arg(mu) / (2 * M_PI);
                        if (turns < 0) turns += 1;
                        const double mk = turns * k_try;
                        const int m = static_cast<int>(std::lround(mk));
                        if (std::abs(std::pow(mu, k_try) - 1.0) < PARENT_TOLERANCE && std::abs(mk - m) < 0.1 && m > 0 && m < k_try
                                && gcd(m, k_try) == 1) {
                            parent_found(mu, m);
                        } else {
                            next_divisor();
                        }
                    } else if (!done) {
                        begin(q, true);
                    } else {
                        next_divisor();
                    }
                }
                break;
            case IDLE:
                break;
        }
    }
    return phase != IDLE;
}

void BulbNamer::add_candidate(int k) {
    if (k <= 0 || candidate_count >= MAX_CANDIDATES) return;
    for (int i = 0; i < candidate_count; ++i) {
        if (candidates[i] == k) return;
    }
    candidates[candidate_count++] = k;
}

void BulbNamer::try_candidates() {
    std::sort(candidates, candidates + candidate_count);
    candidate_index = 0;
    next_candidate();
}

void BulbNamer::next_candidate() {
    if (candidate_index >= candidate_count) {
        phase = IDLE;
        return;
    }
    zr = start_zr;
    zi = start_zi;
    newton_steps = 0;
    last_step = INFINITY;
    begin(candidates[candidate_index++], true);
    phase = PERIOD;
}

/**
 * One step of Newton's method for f^length(z) = z, the orbit is done. Returns false if it failed. done: converged
 * (z is the solution, A its multiplier) or failed.
 */
bool BulbNamer::newton_z(bool& done) {
    const complex residual = distance();
    const complex change = -residual / (A - 1.0);
    const double size = std::abs(change), r = std::abs(residual);
    if (newton_steps == 0) first_residual = r;
    done = true;
    if (!finite(change) || size > 4) return false;
    const bool stalled = newton_steps >= 2 && size > 0.5 * last_step;  // at the precision limit
    if (size == 0 || stalled) return r <= CONVERGED * first_residual || r < TINY;
    if (++newton_steps > MAX_NEWTON_STEPS) return false;
    last_step = size;
    add(zr, zi, change);
    done = false;
    return true;
}

void BulbNamer::found_period(int period, const complex& multiplier) {
    period_ = p = period;
    lambda_from = multiplier;
    if (p == 1) {
        base = 1;
        named_ = true;
        phase = IDLE;
        return;
    }
    start_path();
}

void BulbNamer::start_path() {
    path_step = 0;
    next_target();
}

// The path goes in a straight line through the disc of multipliers, in smaller steps towards the end
void BulbNamer::next_target() {
    path_step++;
    const double u = 1.0 - static_cast<double>(path_step) / PATH_STEPS;
    target = lambda_from + (ROOT_MULTIPLIER - lambda_from) * (1.0 - u * u);
    newton_steps = 0;
    last_step = INFINITY;
    begin(p, true);
    phase = ROOT;
}

// Newton's method for z and c: f^p(z) - z = 0 and (f^p)'(z) - target = 0
void BulbNamer::root_step() {
    const complex f1 = distance(), f2 = A - target;
    const complex a = A - 1.0;
    const complex det = a * D - B * C;
    const complex dz = -(D * f1 - B * f2) / det;
    const complex dc = -(a * f2 - C * f1) / det;
    const double size = std::abs(dc);
    if (!finite(dz) || !finite(dc)) {
        phase = IDLE;
        return;
    }
    if (newton_steps == 0) first_correction = size;
    add(zr, zi, dz);
    add(cr, ci, dc);
    const bool last = path_step == PATH_STEPS;
    // Along the way it only has to be close, at the end precise
    const bool converged = size == 0 || (newton_steps >= 1 && size <= (last ? 1e-10 : 1e-4) * first_correction)
                        || (last && newton_steps >= 2 && size > 0.5 * last_step);
    if (converged) {
        if (last) {
            start_parent();
        } else {
            next_target();
        }
        return;
    }
    if (++newton_steps > MAX_ROOT_STEPS) {
        phase = IDLE;
        return;
    }
    last_step = size;
    begin(p, true);
}

void BulbNamer::start_parent() {
    root_cr = cr;
    root_ci = ci;
    root_zr = zr;
    root_zi = zi;
    k_try = p + 1;
    next_divisor();
}

/**
 * The next possible parent period q = p / k, the shortest first: f^q(z) = z holds for the cycles of the divisors of q
 * as well, and those have been ruled out then.
 */
void BulbNamer::next_divisor() {
    cr = root_cr;
    ci = root_ci;
    while (--k_try >= 2) {
        if (p % k_try != 0) continue;
        q = p / k_try;
        zr = root_zr;
        zi = root_zi;
        sum = 0.0;
        begin(p - q, false);
        phase = CENTROID;
        return;
    }
    // No parent: a cardioid
    base = p;
    named_ = true;
    phase = IDLE;
}

void BulbNamer::parent_found(const complex& mu, int m) {
    bulbs[depth_++] = {m, k_try};
    p = q;
    if (p == 1) {
        base = 1;
        named_ = true;
        phase = IDLE;
        return;
    }
    if (depth_ == MAX_DEPTH) {
        phase = IDLE;
        return;
    }
    // On from the parent's cycle at the root, z is the parent's cycle point
    lambda_from = mu;
    start_path();
}
