#include "AutoZoom.hpp"
#include "globals.h"
#include <algorithm>
#include <cmath>

AutoZoom::AutoZoom(FractalisState* state, Fractalis* fractalis)
    : state(state), fractalis(fractalis) {
        this->randomized_start = false;
    }

void AutoZoom::dive() {
    static bool panned = false;
    static uint16_t skip_counter = 0;
    if (!state->auto_zoom) return;
    state->skip_pre_render = true;

    if (skip_counter > 0) {
        skip_counter--;
        return;
    }


    std::pair<int, int> zoomPoint = identifyCenterOfTileOfDetail();
    if (!panned) {
        initiatePan(zoomPoint.first, zoomPoint.second);
        panned = true;
    } else {
        fractalis->zoom(ZOOM_CONSTANT/1.5L);
        panned = false;
    }
    const uint8_t sleep_s = 1;
    skip_counter = (1000/UPDATE_SLEEP) * sleep_s;
}

std::pair<int, int> AutoZoom::identifyCenterOfTileOfDetail() {
    int maxDetailScore = -1;
    std::pair<int, int> centerOfHighDetail(0, 0);

    int numTilesX = state->screen_w / TILE_SIZE;
    int numTilesY = state->screen_h / TILE_SIZE;

    for (int tileY = 0; tileY < numTilesY; ++tileY) {
        for (int tileX = 0; tileX < numTilesX; ++tileX) {
            int detailScore = measureTileDetail(tileX, tileY);

            // Apply center bias
            double centerDistanceX = std::abs(tileX - numTilesX / 2.0) / (numTilesX / 2.0);
            double centerDistanceY = std::abs(tileY - numTilesY / 2.0) / (numTilesY / 2.0);
            double centerBias = 1.0 + (1.0 - std::max(centerDistanceX, centerDistanceY)) * (CENTER_BIAS - 1.0);

            detailScore = static_cast<int>(detailScore * centerBias);

            if (detailScore > maxDetailScore) {
                maxDetailScore = detailScore;
                centerOfHighDetail = calculateCenter(tileX, tileY);
            }
        }
    }

    return centerOfHighDetail;
}

void AutoZoom::initiatePan(int x, int y) {
    double panX = (x - state->screen_w / 2) / static_cast<double>(state->screen_w) * PAN_CONSTANT;
    double panY = (y - state->screen_h / 2) / static_cast<double>(state->screen_h) * PAN_CONSTANT;

    if (!this->randomized_start) {
        const double max = 1.0;
        const double min = -max;
        float random_x = ((float) rand()) / (float) RAND_MAX;
        float random_y = ((float) rand()) / (float) RAND_MAX;

        const float range = max - min;

        panX += (random_x*range) + min;
        panY += (random_y*range) + min;
        printf("Panning with randomized coordinates: x:%f, y:%f\n", panX, panY);
        this->randomized_start = true;
    }

    fractalis->pan(panX, panY);
}

std::pair<int, int> AutoZoom::calculateCenter(int tileX, int tileY) {
    return std::make_pair(
        tileX * TILE_SIZE + TILE_SIZE / 2,
        tileY * TILE_SIZE + TILE_SIZE / 2
    );
}

int AutoZoom::measureTileDetail(int tileX, int tileY) {
    return countPixelChanges(tileX, tileY);
}

int AutoZoom::countPixelChanges(int tileX, int tileY) {
    int changeCount = 0;
    int startX = tileX * TILE_SIZE;
    int startY = tileY * TILE_SIZE;

    for (int y = 0; y < TILE_SIZE; ++y) {
        for (int x = 0; x < TILE_SIZE; ++x) {
            int currentX = startX + x;
            int currentY = startY + y;

            if (currentX >= state->screen_w || currentY >= state->screen_h) continue;

            uint16_t currentIteration = state->pixelState[currentY][currentX].iteration;

            if (x > 0) {
                uint16_t leftIteration = state->pixelState[currentY][currentX - 1].iteration;
                if (currentIteration != leftIteration) changeCount++;
            }
            if (y > 0) {
                uint16_t topIteration = state->pixelState[currentY - 1][currentX].iteration;
                if (currentIteration != topIteration) changeCount++;
            }
        }
    }

    return changeCount;
}