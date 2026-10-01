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
    void dive(uint32_t now_ms, bool calculating);
    void next_speed();
    const char* speed_name() const;
    // Call when auto zoom gets switched on. It continues from the current view.
    void start();
    std::pair<int, int> identifyCenterOfTileOfDetail(int& detail_score);
    void initiatePan(int x, int y);

private:
    FractalisState* state;
    Fractalis* fractalis;
    bool randomized_start;
    uint32_t next_step_ms;
    int speed;

    struct Speed {
        const char* name;
        double zoom_factor;  // per step
        uint32_t pause_ms;   // time to look at the image before the next step
        // Next step once a pass with this fraction of the full iteration limit covered the screen (1 = all done)
        float detail;
    };
    static const Speed SPEEDS[];
    static const int SPEED_COUNT;

    static constexpr int TILE_SIZE = 32;  // Size of tiles for detail analysis
    static constexpr double CENTER_BIAS = 1.5;  // Bias factor for center tiles
    // All the way down, until the precision runs out
    static constexpr double MAX_ZOOM = DOUBLE_DOUBLE_MAX_ZOOM;

    std::pair<int, int> calculateCenter(int tileX, int tileY);
    int measureTileDetail(int tileX, int tileY);
};

#endif // AUTO_ZOOM_H
