#ifndef FRACTALIS_H
#define FRACTALIS_H

#include "FractalisState.h"
#include "doubledouble.h"
#include "sync.h"

using namespace doubledouble;

/**
 * Calculates the fractal. All public methods are safe to call from both cores.
 *
 * The pixels of a calculation pass are handed out one by one in concentric rings starting at the screen center,
 * so any number of cores can work on the same pass. View changes (pan/zoom) increment state->calculation_id,
 * results that were calculated for an older view are thrown away.
 */
class Fractalis {
public:
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

    /**
     * Supersampling: once a view is calculated, every pixel gets samples - 1 more sub-samples. 1 (off), 2, 3, 4, 6
     * or 8. Changing it recalculates the view. Not done during auto zoom, unless it waits for full quality.
     */
    void set_supersampling(int samples);
    int supersampling() const { return ss_samples; }
    // Supersamples the current view, if it is done but not supersampled yet (e.g. after auto zoom stopped)
    void supersample();

    // The view and pixel state must not be modified by others while the lock is held
    Lock& get_lock() { return lock; }

    // Iteration limit for a full calculation pass at the given zoom
    int max_iterations(double zoom) const;

private:
    struct View {
        Coordinate center;
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
        bool minibrot;
        double nucleus_offset_r, nucleus_offset_i;  // center - nucleus
        bool cardioid;
        double scale_r, scale_i;
        // Everything this close to the reference is in the set (0 = unknown)
        double interior_radius_sq;
    };

    /**
     * Reference orbit for perturbation: Z_0 = 0, Z_n+1 = Z_n^2 + C, calculated in double-double and stored in
     * single precision (re, im interleaved). It is calculated step by step up to the iteration limit of the pass
     * and kept as long as C is on or near the screen, so zooming and panning usually don't need a new one.
     */
    struct Reference {
        float* orbit;
        int length;        // stored values
        bool escaped;      // the orbit escaped at the last stored value and can't be extended
        Coordinate c;
        DoubleDouble zr, zi;  // last value, to continue the orbit
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
    int pass_resolved;  // pixels that escaped in the current pass
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

    void start_pass();
    void begin_pass(int limit, int target);
    int next_pass_limit(int limit, int target) const;
    void finish_pass();
    bool claim_pixel(int& x, int& y);
    void store_result(int x, int y, const PixelState& result);
    void request_calculation();

    bool reference_ready() const;
    // Picks C for a new reference orbit: the undecided pixel closest to the center, else the slowest one to escape
    void choose_reference();
    void set_reference(const Coordinate& c);
    // Calculates the reference orbit up to the iteration limit of the pass, without holding the lock
    // Returns false if interrupted
    bool extend_reference(uint32_t id, int target_length, bool (*interrupt)());

    // Supersampling, see set_supersampling()
    int ss_samples = 1;
    bool ss_pass = false;  // the current pass adds sub-samples
    void begin_supersampling();
    // Pixels the current pass still has to do
    bool needs_work(const PixelState& pixel) const;

    // Returns false, if the calculation was aborted. sub_x/sub_y: sample position in the pixel, 0 = center.
    bool calculate_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                         PixelState& pixel, float sub_x = 0.0f, float sub_y = 0.0f) const;
    // Adds the sub-samples to a calculated pixel
    bool supersample_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                           PixelState& pixel) const;
};

#endif // FRACTALIS_H
