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
     * Call regularly while no calculation is running. After a short pause it alternates between
     * panning towards the area with the most detail and zooming in.
     */
    void dive(uint32_t now_ms);
    // Call when auto zoom gets switched on. It continues from the current view.
    void start();
    std::pair<int, int> identifyCenterOfTileOfDetail(int& detail_score);
    void initiatePan(int x, int y);

private:
    FractalisState* state;
    Fractalis* fractalis;
    bool randomized_start;
    bool panned;
    uint32_t next_step_ms;

    static constexpr int TILE_SIZE = 32;  // Size of tiles for detail analysis
    static constexpr double CENTER_BIAS = 1.5;  // Bias factor for center tiles
    static constexpr uint32_t PAUSE_MS = 1000;  // Time to look at the finished image
    // Zooming deeper would switch to the very slow DoubleDouble calculation, stop there
    static constexpr double MAX_ZOOM = DOUBLE_MAX_ZOOM;

    std::pair<int, int> calculateCenter(int tileX, int tileY);
    int measureTileDetail(int tileX, int tileY);
};

#endif // AUTO_ZOOM_H
