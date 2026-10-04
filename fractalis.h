#ifndef FRACTALIS_H
#define FRACTALIS_H

#include "FractalisState.h"
#include "globals.h"
#include "sync.h"

/**
 * Calculates the fractal. All public methods are safe to call from both cores.
 *
 * The pixels of a calculation pass are handed out one by one in concentric rings starting at the screen center,
 * so any number of cores can work on the same pass. View changes (pan/zoom) increment state->calculation_id,
 * results that were calculated for an older view are thrown away.
 */
class Fractalis {
public:
    // Orbit trap shapes, see set_orbit_trap()
    // TRAP_PERIOD is the period map: colors by the period of the atom domain, the iteration where |z| came closest to 0
    // TRAP_STRIPE is stripe average coloring: the average of sin(4 * angle of z) over the orbit
    enum Trap : int { TRAP_OFF, TRAP_POINT, TRAP_CROSS, TRAP_RING, TRAP_PERIOD, TRAP_STRIPE, TRAP_COUNT };
    // Where the trap colors: the rest gets the usual coloring (smooth iteration count outside, black inside)
    enum TrapRegion : int { REGION_EVERYWHERE, REGION_INSIDE, REGION_OUTSIDE, REGION_COUNT };
    // Iterations for the trap of pixels the interior checks found in the set without iterating
    static constexpr int TRAP_INTERIOR_ITER = 500;

    Fractalis(FractalisState* state);
    ~Fractalis();
    Fractalis(const Fractalis&) = delete;
    Fractalis& operator=(const Fractalis&) = delete;

    /**
     * Calculates a few pixels of the current pass.
     * @param interrupt optional, checked regularly during long calculations. Returning true stops the work early,
     *                  unfinished pixels are handed to the next free core.
     * @return false if there is currently nothing to do or the work was interrupted.
     */
    bool work(bool (*interrupt)() = nullptr);

    void zoom(double factor);
    /**
     * @brief Pan the fractal view by the given amount. Rounded to full pixels.
     * @param dx The amount to pan in the x direction as fraction of the screen width. between -1-1.
     * @param dy The amount to pan in the y direction as fraction of the screen height. between -1-1.
     */
    void pan(double dx, double dy);
    void reset_view();
    // Jumps to the view, e.g. a saved one
    void set_view(const Coordinate& center, double zoom);
    // Moves the center there, by any amount. Close by, the image stays as a preview while it is calculated again.
    void move_to(const Coordinate& center);

    /**
     * Supersampling: once a view is calculated, every pixel gets samples - 1 more sub-samples. 1 (off), 2, 3, 4, 6
     * or 8. Changing it recalculates the view. Not done during auto zoom, unless it waits for full quality.
     */
    void set_supersampling(int samples);
    int supersampling() const { return ss_samples; }
    // Supersamples the current view, if it is done but not supersampled yet (e.g. after auto zoom stopped)
    void supersample();

    /**
     * Orbit trap coloring: the color comes from how close the orbit gets to a shape (the origin, the axes or the
     * unit circle), inside the set as well, or the period map (TRAP_PERIOD). Changing it recalculates the view.
     */
    void set_orbit_trap(int trap) { set_orbit_trap(trap, trap_region); }
    void set_orbit_trap(int trap, int region);
    int orbit_trap() const { return trap_mode; }
    int orbit_trap_region() const { return trap_region; }

    /**
     * Supersampling right away: every pixel gets all of its sub-samples in every pass, the probes as well. No
     * separate supersampling pass at the end: slower until the first image, but it doesn't change its look when it's
     * done. Changing it recalculates the view.
     */
    void set_supersample_right_away(bool on);
    bool supersample_right_away() const { return ss_right_away; }
    // The probes are drawn as soon as they are calculated. Otherwise they only measure the iteration limit, and the
    // passes draw their pixels like all others.
    void set_show_probes(bool on) { show_probes_ = on; }
    bool show_probes() const { return show_probes_; }
    /**
     * Pixels that haven't escaped at the limit of the current pass are shown as part of the set (black): the set
     * starts out too big and shrinks with every pass. Otherwise they keep their preview color until the last pass.
     */
    void set_undecided_in_set(bool on) { undecided_in_set_ = on; }
    bool undecided_in_set() const { return undecided_in_set_; }

    // The view and pixel state must not be modified by others while the lock is held
    Lock& get_lock() { return lock; }

    // Microseconds since some point, for the statistics printed when a view is done. nullptr: no statistics.
    static uint64_t (*clock_us)();
    // Core cycles since some point (DWT), to measure the cycles per iteration. nullptr: not measured.
    static uint32_t (*cycle_counter)();

    // Statistics of the current view so far, or of the last one once it is done. Times need clock_us.
    struct ViewStats {
        uint32_t ms;
        int passes;
        uint64_t iterations;
        uint32_t cycles_per_iteration;  // 0: not measured
        uint32_t pixels;                // calculated, sub-samples and repeated ones included
        int ref_orbits;
        uint32_t ref_ms;
    };
    ViewStats view_stats();

    // Iteration limit for a full calculation pass at the given zoom, estimated without looking at the view
    int max_iterations(double zoom) const;
    // Iteration limit the current view aims for, measured with the probe points
    int target_iterations() const { return pass_target; }

private:
    struct View {
        Coordinate center;
        // The center as double-double (center_r + center_r_low), for the iterations without perturbation
        double center_r, center_r_low, center_i, center_i_low;
        double zoom;
        double step;  // size of one pixel in the complex plane
        // Single precision copies for the float calculation. The center is split in a high and low part, so c is
        // only rounded once and doesn't depend on how the center was reached.
        float center_rf, center_rf_low, center_if, center_if_low, step_f;
        // Perturbation: pixels iterate their difference to the reference orbit
        bool perturbed;
        double ref_offset_r, ref_offset_i;  // center - reference point
        const float* orbit;
        int orbit_length;
        // Hyperbolic component (minibrot or bulb) the reference is in, pixels safely inside are in the set
        bool minibrot = false;
        double nucleus_offset_r = 0, nucleus_offset_i = 0;  // center - nucleus
        bool cardioid = false;
        double scale_r = 0, scale_i = 0;
        // Everything this close to the reference is in the set (0 = unknown)
        double interior_radius_sq = 0;
        int trap = TRAP_OFF;
        int trap_region = REGION_EVERYWHERE;
        // Series approximation: dz at iteration series_skip = sum series[k] dc^(k+1), the pixels start there
        int series_skip = 0;
        double series_r[4] = {}, series_i[4] = {};
    };

    /**
     * Reference orbit for perturbation: Z_0 = 0, Z_n+1 = Z_n^2 + C, calculated in fixed point and stored in
     * single precision (re, im interleaved). It is calculated step by step up to the iteration limit of the pass
     * and kept as long as C is on or near the screen, so zooming and panning usually don't need a new one.
     */
    struct Reference {
        float* orbit;
        int length;        // stored values
        bool escaped;      // the orbit escaped at the last stored value and can't be extended
        Coordinate c;
        Fixed zr, zi;      // last value, to continue the orbit
        uint32_t generation;  // incremented when C changes
        bool busy;            // a core is calculating the orbit (outside of the lock)
        // C candidates of the current pass that escaped too early, in screen pixels
        static constexpr int MAX_TRIES = 4;
        int tries;
        int16_t tried_x[MAX_TRIES], tried_y[MAX_TRIES];
        int best_length;
        Coordinate best_c;
        // Component of the reference, see View
        enum : uint8_t { MINIBROT_UNKNOWN, MINIBROT_FOUND, MINIBROT_NONE } minibrot;
        int minibrot_tried_length;  // orbit length at the last search, it's tried again with a longer orbit
        int period;
        Coordinate nucleus;
        bool component_check;  // the reference is safely inside the component, the check is worth it
        bool cardioid;
        double scale_r, scale_i;
        double interior_radius;
    };

    FractalisState* state;
    Lock lock;
    Reference ref;

    // Current pass, protected by the lock
    uint32_t pass_id;
    int pass_limit;
    int pass_target;    // iteration limit to reach with the current view
    int view_target;    // the target measured with the probes, refining goes up to REFINE_MAX_FACTOR times that
    int last_view_limit = 0;  // highest limit of the last finished view, the probes of the next one go to twice that
    // Undecided pixels of the float calculation store z in the (then unused) orbit buffer, the next pass continues
    // there
    int resume_limit = 0;     // limit of the pass before, where the stored z are from
    int resume_slots = 0;     // used slots
    // Series approximation of the current view, valid for this reference generation
    uint32_t series_generation = 0;
    void compute_series();

    // Where the time of a view goes, printed when it is done
    struct Stats {
        uint64_t view_start, pass_start;
        uint64_t view_end;  // 0 while the view is calculated
        int pass_count;
        int pass_pixels;
        int ref_orbits, ref_iterations;
        uint64_t ref_us, search_us;
        char passes[200];  // "limit:ms/pixels " per pass
        int passes_length;
        uint64_t cycles, iterations;  // in the pixel calculations, all cores
        uint32_t pixels;
    } stats;
    uint64_t now_us() const { return clock_us ? clock_us() : 0; }
    void log_pass(const char* name);
    // The view is done: stops its clock and prints the statistics
    void finish_stats();
    int pass_resolved;  // pixels that escaped in the current pass
    int pass_ran_out;   // pixels that reached the limit of the current pass without being proven to be in the set
    View pass_view;
    int next_index;
    int in_flight;

    static constexpr int MAX_BATCH = 16;
    static constexpr int RETURNED_CAPACITY = 4 * MAX_BATCH;
    struct PixelPosition { int16_t x, y; };
    PixelPosition returned[RETURNED_CAPACITY];
    int returned_count;
    // Iteration limit for the first pass of a new view, estimated from the previous view. 0 = unknown
    int first_limit_hint;
    int estimate_first_limit() const;

    /**
     * Probe points: every view starts with a pass over a few hundred pixels at the highest limit (MAX_ITER), dense
     * at the center and sparse further out. Their escape iterations set the target limit of the view. They are
     * normal pixels of the image.
     */
    static constexpr int MAX_PROBES = 400;
    PixelPosition probes[MAX_PROBES];  // offsets from the screen center
    int probe_count;
    bool probe_pass = false;
    // The probes that escaped. Weight: the area a probe stands for (the square of the grid spacing there), so the
    // dense probes at the center don't dominate.
    struct ProbeResult {
        uint16_t iterations;
        uint16_t weight;
    };
    ProbeResult probe_results[MAX_PROBES];
    int probe_escaped;
    // Iterations by which this share of the escaping probes (by area) escaped
    int probe_percentile(float share) const;
    int probe_ran_out;
    void begin_view_passes();

    // Optional extra output of calculate_pixel()
    struct PixelInfo {
        int iterations;  // until it escaped (or the limit)
        int work;        // iterations actually done (without the skipped and continued ones)
        bool ran_out;    // in the set at this limit, but not proven to be in the set
        // In: continue from z at iteration n. Out: ran out with z, it can continue from there.
        bool has_state;
        float zr, zi;
        int n;
    };

    void start_pass();
    void begin_pass(int limit, int target);
    int next_pass_limit(int limit, int target) const;
    void finish_pass();
    bool claim_pixel(int& x, int& y);
    // The order the pixels of a pass are handed out in: coarse to fine grids, from the center outwards
    int order_p = 0, order_step = 0, order_k = 0;
    bool next_in_order(int& x, int& y);
    void store_result(int x, int y, const PixelState& result, const PixelInfo& info);
    void request_calculation();

    bool reference_ready() const;
    // Picks C for a new reference orbit: the undecided pixel closest to the center, else the slowest one to escape
    void choose_reference();
    void set_reference(const Coordinate& c);
    // Calculates the reference orbit up to the iteration limit of the pass, without holding the lock
    // Returns false if interrupted
    bool extend_reference(uint32_t id, int target_length, bool (*interrupt)());

    int trap_mode = TRAP_OFF;
    int trap_region = REGION_EVERYWHERE;
    bool ss_right_away = false;
    bool show_probes_ = false;
    bool undecided_in_set_ = UNDECIDED_IN_SET;
    // The current pass supersamples every pixel right away
    bool right_away_pass() const { return ss_right_away && ss_samples > 1 && !ss_pass; }
    // Supersampling, see set_supersampling()
    int ss_samples = 1;
    bool ss_pass = false;  // the current pass adds sub-samples
    void begin_supersampling();
    // Pixels the current pass still has to do
    bool needs_work(const PixelState& pixel) const;

    // Returns false, if the calculation was aborted. sub_x/sub_y: sample position in the pixel, 0 = center.
    bool calculate_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                         PixelState& pixel, float sub_x = 0.0f, float sub_y = 0.0f, PixelInfo* info = nullptr) const;
    // Adds the sub-samples to a calculated pixel
    bool supersample_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                           PixelState& pixel) const;
};

#endif // FRACTALIS_H
