#include "AutoZoom.hpp"
#include "globals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

const AutoZoom::Speed AutoZoom::SPEEDS[] = {
    {"x1.05", 1.05, 500, 1.0f},
    {"x1.1", 1.1, 500, 1.0f},
    {"x1.25", 1.25, 500, 1.0f},
    {"x1.5", 1.5, 500, 1.0f},
    {"x2", 2.0, 500, 1.0f},
    {"fly", 1.04, 0, 0.25f},  // continuous: every frame finishes, but with less detail
};
const int AutoZoom::SPEED_COUNT = sizeof(SPEEDS) / sizeof(SPEEDS[0]);

AutoZoom::AutoZoom(FractalisState* state, Fractalis* fractalis)
    : state(state), fractalis(fractalis), randomized_start(false), next_step_ms(0), speed(1) {}

// zoom() takes the relative change, e.g. 0.1 for x1.1
static double zoom_in_change(double factor) { return factor - 1.0; }
static double zoom_out_change(double factor) { return 1.0 / factor - 1.0; }

void AutoZoom::start() {
    // A random first step only from the overview, otherwise every dive would look the same
    randomized_start = state->zoom_factor >= 2.0;
    next_step_ms = 0;
}

void AutoZoom::next_speed() {
    speed = (speed + 1) % SPEED_COUNT;
    next_step_ms = 0;
}

const char* AutoZoom::speed_name() const {
    return SPEEDS[speed].name;
}

void AutoZoom::dive(uint32_t now_ms, bool calculating) {
    const Speed& s = SPEEDS[speed];
    int needed_limit = static_cast<int>(s.detail * fractalis->max_iterations(state->zoom_factor));
    bool ready = !calculating || (s.detail < 1.0f && state->completed_limit >= needed_limit);
    if (!state->auto_zoom || !ready) {
        next_step_ms = 0;
        return;
    }
    // Pause before every step
    if (next_step_ms == 0) {
        next_step_ms = now_ms + s.pause_ms;
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
        fractalis->zoom(zoom_out_change(s.zoom_factor));
    } else {
        initiatePan(zoomPoint.first, zoomPoint.second);
        fractalis->zoom(zoom_in_change(s.zoom_factor));
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
        if (!a.isValid() || !b.isValid()) return 0;
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
