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

    void dive();
    std::pair<int, int> identifyCenterOfTileOfDetail();
    void initiatePan(int x, int y);

private:
    FractalisState* state;
    Fractalis* fractalis;

    static constexpr int TILE_SIZE = 32;  // Size of tiles for detail analysis
    static constexpr double CENTER_BIAS = 1.2;  // Bias factor for center tiles

    std::pair<int, int> calculateCenter(int tileX, int tileY);
    int measureTileDetail(int tileX, int tileY);
    int countPixelChanges(int tileX, int tileY);
};

#endif // AUTO_ZOOM_H