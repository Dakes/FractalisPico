#ifndef BULB_NAMER_H
#define BULB_NAMER_H

#include "FractalisState.h"
#include <complex>
#include <cstdint>

/**
 * Names the component of the set a point lies in: "the 1/2 bulb of the 1/3 bulb of the main cardioid", "the 2/5 bulb
 * of a period 184 minibrot". It works in small slices (see work()), like MinibrotFinder.
 *
 * 1. Period: Newton's method for a cycle of the point, for a few candidate lengths (where the orbit comes back close
 *    to itself). An attracting cycle (multiplier below 1) means the point is in a component of that period.
 * 2. Root: the multiplier (f^p)'(z) maps the component onto the unit disc, its root (where a bulb is attached to its
 *    parent, or the cusp of a cardioid) is at multiplier 1. Newton's method for z and c with f^p(z) = z and
 *    (f^p)'(z) = multiplier gets there along a path of multipliers.
 * 3. Parent: at the root of a bulb, k points of its cycle merge into one of the parent's cycle, period p / k. The
 *    parent's multiplier there is e^(2 pi i m / k): it's the m/k bulb of the parent. A cardioid has no parent: it's
 *    the main cardioid (period 1) or a minibrot.
 * 4. The same from the parent, until a cardioid.
 */
class BulbNamer {
public:
    static constexpr int MAX_DEPTH = 6;
    struct Bulb {
        int m, k;  // the m/k bulb
    };

    /**
     * c: the point. z: its orbit after many iterations, close to the cycle if it has one. hint: a cycle length to try
     * first, 0 = none.
     */
    void start(const Coordinate& c, const Fixed& zr, const Fixed& zi, int hint);
    void stop() { phase = IDLE; }
    bool busy() const { return phase != IDLE; }
    // Works for about this many iterations. Returns true while busy.
    bool work(int iterations);

    // Period of the component the point is in, 0 = none found (yet)
    int period() const { return period_; }
    // The chain is complete, down to a cardioid
    bool named() const { return named_; }
    // Bulbs from the point's component outwards: bulb(0) is the one the point is in, bulb(depth() - 1) is attached to
    // the cardioid. 0 = the point is in the cardioid itself.
    int depth() const { return depth_; }
    const Bulb& bulb(int i) const { return bulbs[i]; }
    // Period of the cardioid: 1 = the main cardioid, more = a minibrot
    int base_period() const { return base; }

private:
    using complex = std::complex<double>;
    static constexpr int MAX_CANDIDATES = 4;
    enum Phase : uint8_t { IDLE, SEARCH, SEARCH_AGAIN, PERIOD, SCAN, ROOT, CENTROID, PARENT };
    Phase phase = IDLE;
    uint32_t total = 0;

    // The point being worked on (moves to the roots) and a point of its cycle
    Fixed cr, ci, zr, zi;
    Fixed start_zr, start_zi;
    int p = 0;  // period of the component being worked on

    // The orbit being followed: z_n from z_0 = (zr, zi), and the derivatives of z_n by z_0 (A, C: second) and c (B,
    // D: by z_0 and c)
    Fixed er, ei;
    int n = 0, length = 0;
    bool derivatives = false;
    complex A, B, C, D;
    void begin(int steps, bool with_derivatives);
    bool step();
    complex distance() const;  // z_n - z_0

    // Cycle lengths to try
    double best_distance = 0;
    int best_k = 0, first_close = 0;
    int candidates[MAX_CANDIDATES];
    int candidate_count = 0, candidate_index = 0;
    void add_candidate(int k);
    void try_candidates();
    void next_candidate();

    // Newton's method
    int newton_steps = 0;
    double last_step = 0, first_residual = 0, scan_tolerance = 0;
    bool newton_z(bool& done);  // one step for f^length(z) = z with c fixed

    // Path of multipliers to the root
    complex lambda_from, target;
    int path_step = 0;
    double first_correction = 0;
    void start_path();
    void next_target();
    void root_step();

    // Parent
    Fixed root_cr, root_ci, root_zr, root_zi;
    int k_try = 0, q = 0;
    complex sum;
    void start_parent();
    void next_divisor();
    void parent_found(const complex& mu, int m);

    int period_ = 0, depth_ = 0, base = 0;
    bool named_ = false;
    Bulb bulbs[MAX_DEPTH];
    void found_period(int period, const complex& multiplier);
};

#endif // BULB_NAMER_H
