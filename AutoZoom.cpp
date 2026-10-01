#include "AutoZoom.hpp"
#include "globals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

AutoZoom::AutoZoom(FractalisState* state, Fractalis* fractalis)
    : state(state), fractalis(fractalis), randomized_start(false), panned(false), next_step_ms(0) {}

void AutoZoom::start() {
    // A random first step only from the overview, otherwise every dive would look the same
    randomized_start = state->zoom_factor >= 2.0;
    panned = false;
    next_step_ms = 0;
}

void AutoZoom::dive(uint32_t now_ms) {
    if (!state->auto_zoom) {
        next_step_ms = 0;
        return;
    }
    // Pause after every finished calculation
    if (next_step_ms == 0) {
        next_step_ms = now_ms + PAUSE_MS;
        return;
    }
    if (static_cast<int32_t>(now_ms - next_step_ms) < 0) {
        return;
    }
    next_step_ms = 0;

    if (state->zoom_factor >= MAX_ZOOM) {
        printf("Auto zoom reached max zoom, stopping\n");
        state->auto_zoom = false;
        state->needs_redraw = true;
        return;
    }

    int detail_score;
    std::pair<int, int> zoomPoint = identifyCenterOfTileOfDetail(detail_score);
    if (detail_score == 0) {
        // Nothing interesting on screen (e.g. inside the set), back out
        fractalis->zoom(-ZOOM_CONSTANT);
        panned = false;
    } else if (!panned) {
        initiatePan(zoomPoint.first, zoomPoint.second);
        panned = true;
    } else {
        fractalis->zoom(ZOOM_CONSTANT / 1.5);
        panned = false;
    }
}

std::pair<int, int> AutoZoom::identifyCenterOfTileOfDetail(int& detail_score) {
    int maxDetailScore = 0;
    std::pair<int, int> centerOfHighDetail(state->screen_w / 2, state->screen_h / 2);

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

    detail_score = maxDetailScore;
    return centerOfHighDetail;
}

void AutoZoom::initiatePan(int x, int y) {
    double panX = (x - state->screen_w / 2) / static_cast<double>(state->screen_w) * PAN_CONSTANT;
    double panY = (y - state->screen_h / 2) / static_cast<double>(state->screen_h) * PAN_CONSTANT;

    if (!this->randomized_start) {
        const double max = 0.35;
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

/**
 * Sums up the color differences between neighboring pixels. With smooth coloring nearly all neighbors differ a
 * little, so big jumps (and the border of the set) count the most.
 */
int AutoZoom::measureTileDetail(int tileX, int tileY) {
    static constexpr int MAX_DIFFERENCE = 256;
    auto difference = [](const PixelState& a, const PixelState& b) {
        if (!a.isComplete() || !b.isComplete()) return 0;
        if (a.isInSet() && b.isInSet()) return 0;
        if (a.isInSet() != b.isInSet()) return MAX_DIFFERENCE;
        return std::min(std::abs(static_cast<int>(a.color) - static_cast<int>(b.color)), MAX_DIFFERENCE);
    };

    int score = 0;
    int startX = tileX * TILE_SIZE;
    int startY = tileY * TILE_SIZE;

    for (int y = startY; y < std::min(startY + TILE_SIZE, state->screen_h); ++y) {
        for (int x = startX; x < std::min(startX + TILE_SIZE, state->screen_w); ++x) {
            const PixelState& current = state->pixelState[y][x];
            if (x > startX) score += difference(current, state->pixelState[y][x - 1]);
            if (y > startY) score += difference(current, state->pixelState[y - 1][x]);
        }
    }

    return score;
}
