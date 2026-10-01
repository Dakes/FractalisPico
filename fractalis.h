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
    };

    FractalisState* state;
    Lock lock;

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

    // Returns false, if the calculation was aborted
    bool calculate_pixel(int x, int y, const View& view, int iter_limit, uint32_t id, bool (*interrupt)(),
                         PixelState& pixel) const;
};

#endif // FRACTALIS_H
