#ifndef MINIBROT_FINDER_H
#define MINIBROT_FINDER_H

#include "FractalisState.h"
#include <cmath>
#include <complex>
#include <cstdint>

/**
 * Finds a minibrot near a point, of a size that fills the screen at a given zoom. It works in small slices (see
 * work()), so it can run next to everything else.
 *
 * 1. From the start point (outside of the set) along the field line towards the set, with the exterior distance
 *    estimate, until the point is much closer to the set than the minibrot will be big.
 * 2. Ball method: the first iteration n where the disc of radius r around the point maps onto a disc around 0
 *    (|z_n| < r |dz_n/dc|) is the lowest period in the disc, the period of the biggest minibrot in it. Newton's
 *    method finds its nucleus, the size estimate of Claude Heiland-Allen its size: about r^2 / 16.
 * 3. r is adjusted until the size fits the zoom.
 * Minibrots get smaller step by step towards a point on the edge of the set, so there's one every one to three
 * powers of ten of zoom.
 */
class MinibrotFinder {
public:
    static constexpr int MAX_STARTS = 4;

    struct Result {
        Coordinate nucleus;
        int period;
        // (c - nucleus) * scale are the coordinates in the minibrot, like those of the whole set. |scale| = 1 / size:
        // the zoom where the minibrot fills the screen like the whole set at zoom 1.
        std::complex<double> scale;
        double zoom() const { return std::abs(scale); }
    };

    /**
     * Starts a search for a minibrot that fills the screen at zoom target. starts: points outside of the set to
     * start from, the preferred one first. The next one is only tried if the minibrots found are too far off in
     * size.
     */
    void start(const Coordinate* starts, int count, double target);
    /**
     * Only the ball method and Newton's method, for the disc of radius r around c: the biggest minibrot in it, any
     * size. found() tells if there is one.
     */
    void probe(const Coordinate& c, double r);
    void stop() { phase = IDLE; }
    bool busy() const { return phase != IDLE; }
    // Works for about this many iterations (in fixed point, a few microseconds each). Returns true while busy.
    bool work(int iterations);
    // The best minibrot so far, if it's at least roughly the size (within 3 powers of ten of the zoom)
    bool found() const { return best_error <= 3; }
    const Result& result() const { return best; }
    double target() const { return target_zoom; }
    // Work done so far
    uint32_t iterations() const { return total_iterations; }
    int probes() const { return probe_count; }

private:
    enum Phase : uint8_t { IDLE, WALK, BALL, NEWTON };
    Phase phase = IDLE;
    double target_zoom = 0;
    Coordinate starts[MAX_STARTS];
    int start_count = 0;
    int start_index = 0;
    bool single = false;  // probe()
    uint32_t max_iterations = 0;

    // z_n+1 = z_n^2 + c in fixed point, z and dz/dc as double
    struct Orbit {
        Fixed cr, ci, zr, zi;
        std::complex<double> z, dz;
        int n;
        void begin(const Coordinate& c);
        void step();
    } orbit;

    // The walk towards the set: the current point and its distance estimate
    Coordinate point;
    bool have_point;
    double distance;
    std::complex<double> away;  // unit vector, away from the set
    Coordinate trial;           // the next point, being checked
    double walk_factor;         // share of the distance estimate per step
    int walk_steps;
    double walk_goal;           // walks until the distance estimate is below

    // Probes: disc radius, and the radii known to give minibrots too small and too big
    double radius;
    double deep_radius;
    double shallow_radius;
    int start_probes;
    // Newton's method for the nucleus of the period
    int period;
    Coordinate newton_c;
    int newton_steps;
    double newton_delta;  // size of the last step
    // The last step started here. When it overshoots (the orbit escapes), it is shortened.
    Coordinate newton_base;
    std::complex<double> newton_change;
    double newton_damping;
    // Size estimate and shape (see find_minibrot() in fractalis.cpp), along with every orbit of Newton's method
    std::complex<double> l, dl, b, a, a2;
    double divisor_min;  // smallest |z_k|^2 of the proper divisors k of the period

    Result best;
    double best_error = INFINITY;  // |log10(zoom / target)|
    uint32_t total_iterations = 0;
    int probe_count = 0;

    void next_start();
    void step_towards_set();
    void escaped();
    void begin_probe();
    void begin_newton_orbit();
    void newton_step(bool estimated);
    void finish_probe(double zoom);
    void next_radius(double proposed);
    void end_start();
};

#endif // MINIBROT_FINDER_H
