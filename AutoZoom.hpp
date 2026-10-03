#ifndef AUTO_ZOOM_H
#define AUTO_ZOOM_H

#include "FractalisState.h"
#include "fractalis.h"
#include "globals.h"
#include <cstdint>
#include <utility>

class AutoZoom {
public:
    AutoZoom(FractalisState* state, Fractalis* fractalis);

    /**
     * Call regularly. Every step pans towards the area with the most detail and zooms in. Depending on the speed
     * it waits for the calculation to finish first or keeps going (fly).
     */
    void dive(uint32_t now_ms);
    void next_speed();
    const char* speed_name() const;
    int speed_index() const { return speed; }
    void set_speed(int index);
    // Time to look at a finished view before the next step (fly doesn't wait)
    void next_pause();
    const char* pause_name() const;
    int pause_index() const { return pause; }
    void set_pause(int index);
    // Waits for supersampling as well before the next step
    void toggle_full_quality();
    bool full_quality() const { return state->auto_zoom_full_quality; }
    // Seconds until the next step during a pause, 0 otherwise
    uint32_t seconds_to_next_step(uint32_t now_ms) const;
    // Call when auto zoom gets switched on. It continues from the current view.
    void start();
    std::pair<int, int> identifyCenterOfTileOfDetail(int& detail_score);
    // Pans towards the target, so that it gets closer to the center with every zoom step
    void initiatePan(int x, int y, double zoom_factor);

private:
    FractalisState* state;
    Fractalis* fractalis;
    bool randomized_start;
    uint32_t next_step_ms;
    int speed;
    int pause;

    struct Pause {
        const char* name;
        uint32_t ms;
    };
    static const Pause PAUSES[];
    static const int PAUSE_COUNT;

    struct Speed {
        const char* name;
        double zoom_factor;  // per step
        uint32_t pause_ms;   // minimum time to look at the image before the next step
        // Next step once a pass with this fraction of the full iteration limit covered the screen (1 = all done)
        float detail;
    };
    static const Speed SPEEDS[];
    static const int SPEED_COUNT;

    static constexpr int TILE_SIZE = 32;  // Size of tiles for detail analysis
    static constexpr double CENTER_BIAS = 1.5;  // Bias factor for center tiles
    // All the way down, until the precision runs out
    static constexpr double MAX_ZOOM = PRECISION_MAX_ZOOM;

    std::pair<int, int> calculateCenter(int tileX, int tileY);
    int measureTileDetail(int tileX, int tileY);
};

#endif // AUTO_ZOOM_H
