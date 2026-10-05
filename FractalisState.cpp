#include "FractalisState.h"
#include "globals.h"
#include <algorithm>
#include <cmath>
#include <cstring>

FractalisState::FractalisState(int width, int height)
    : screen_w(width), screen_h(height), zoom_factor(1.0), auto_zoom(false), auto_zoom_full_quality(false),
      calculating(0), supersampling(false), calculation_id(0), needs_redraw(false), iteration_limit(25), passes_completed(0), completed_limit(0) {

    center = {-0.5, 0};

    pixelState = new PixelState*[screen_h];
    for (int i = 0; i < screen_h; ++i) {
        pixelState[i] = new PixelState[screen_w];
        for (int j = 0; j < screen_w; ++j) {
            pixelState[i][j].clear();
        }
    }
    content = {0, 0, 0, 0};
    row_buffer = new PixelState[screen_w];
    row_buffer2 = new PixelState[screen_w];
    row_done = new bool[screen_h];
}

FractalisState::~FractalisState() {
    for (int i = 0; i < screen_h; ++i) {
        delete[] pixelState[i];
    }
    delete[] pixelState;
    delete[] row_buffer;
    delete[] row_buffer2;
    delete[] row_done;
}

void FractalisState::store_blends() {
    for (int y = 0; y < screen_h; ++y) {
        for (int x = 0; x < screen_w; ++x) {
            PixelState& p = pixelState[y][x];
            if (p.isValid()) continue;
            uint32_t position;
            Blend blend = interpolated_position(pixelState, screen_w, screen_h, x, y, position);
            if (blend == Blend::COLOR) {
                p.setEscaped(position);
            } else if (blend == Blend::IN_SET) {
                p.setInSet();
            } else {
                // Not the nearest point: as blocks they would pile up into rings with every zoom out
                continue;
            }
            p.markIncomplete();
        }
    }
}

ScreenRect FractalisState::valid_bounds() const {
    ScreenRect r = {screen_w, screen_h, 0, 0};
    for (int y = 0; y < screen_h; ++y) {
        for (int x = 0; x < screen_w; ++x) {
            if (!pixelState[y][x].isValid()) continue;
            r.x0 = std::min(r.x0, x);
            r.y0 = std::min(r.y0, y);
            r.x1 = std::max(r.x1, x + 1);
            r.y1 = std::max(r.y1, y + 1);
        }
    }
    return r;
}

void FractalisState::resetPixelComplete() {
    content = {0, 0, 0, 0};
    zoom_preview = false;
    for (int y = 0; y < screen_h; ++y) {
        for (int x = 0; x < screen_w; ++x) {
            pixelState[y][x].clear();
        }
    }
}

void FractalisState::shiftPixelState(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }
    dx = std::max(-screen_w, std::min(dx, screen_w));
    dy = std::max(-screen_h, std::min(dy, screen_h));
    content = valid_bounds();
    content = {std::max(0, content.x0 + dx), std::max(0, content.y0 + dy),
               std::min(screen_w, content.x1 + dx), std::min(screen_h, content.y1 + dy)};

    // Horizontal shift, row by row
    if (dx != 0) {
        size_t keep = (screen_w - std::abs(dx)) * sizeof(PixelState);
        for (int y = 0; y < screen_h; ++y) {
            PixelState* row = pixelState[y];
            if (dx > 0) {
                memmove(row + dx, row, keep);
                for (int x = 0; x < dx; ++x) row[x].clear();
            } else {
                memmove(row, row - dx, keep);
                for (int x = screen_w + dx; x < screen_w; ++x) row[x].clear();
            }
        }
    }

    // Vertical shift: rotating the row pointers is enough
    if (dy != 0) {
        int shift = ((dy % screen_h) + screen_h) % screen_h;
        std::rotate(pixelState, pixelState + screen_h - shift, pixelState + screen_h);
        int first_new = dy > 0 ? 0 : screen_h + dy;
        int last_new = dy > 0 ? dy : screen_h;
        for (int y = first_new; y < last_new; ++y) {
            for (int x = 0; x < screen_w; ++x) pixelState[y][x].clear();
        }
    }
}

void FractalisState::scalePixelState(double ratio) {
    const float cx = screen_w / 2.0f;
    const float cy = screen_h / 2.0f;
    const float r = static_cast<float>(ratio);
    // Position of the source in the old content, in pixel center coordinates
    auto source = [r](int i, float c) { return (i + 0.5f - c) * r + c - 0.5f; };
    // The content moves with the zoom: the inverse of source()
    auto target = [r](int edge, float c) { return (edge - c) / r + c; };
    // Zooming in, the blended pixels would end up between the grid points. Zooming out they are better left to the
    // live blending: stored, every zoom out would add another ring of coarse blends.
    if (ratio < 1.0) store_blends();
    zoom_preview = true;
    content = valid_bounds();
    content = {std::max(0, static_cast<int>(std::ceil(target(content.x0, cx)))),
               std::max(0, static_cast<int>(std::ceil(target(content.y0, cy)))),
               std::min(screen_w, static_cast<int>(std::floor(target(content.x1, cx)))),
               std::min(screen_h, static_cast<int>(std::floor(target(content.y1, cy))))};

    // Every new pixel is interpolated from the old content (bilinear on the palette position). This is done in
    // place: when zooming in the sources lie closer to the center, so rows are processed from the outside in.
    // When zooming out it is the other way round. Source rows are copied first, so they can't be overwritten while
    // being read, rows that were already overwritten are not used as a source.
    std::fill(row_done, row_done + screen_h, false);
    // Source rows in the buffers. The two rows at the center both need the original of the other one: the second
    // of them reuses the copies of the first.
    int buffered0 = -1, buffered1 = -1;
    for (int n = 0; n < screen_h; ++n) {
        int k = ratio < 1.0 ? n : screen_h - 1 - n;  // distance rank from the outer edge
        int y = (k % 2 == 0) ? k / 2 : screen_h - 1 - k / 2;
        PixelState* row = pixelState[y];
        row_done[y] = true;

        float sy = source(y, cy);
        if (sy < -0.5f || sy > screen_h - 0.5f) {
            for (int x = 0; x < screen_w; ++x) row[x].clear();
            continue;
        }
        int y0 = std::max(0, static_cast<int>(std::floor(sy)));
        int y1 = std::min(screen_h - 1, y0 + 1);
        float fy = std::max(0.0f, std::min(sy - y0, 1.0f));
        if (y0 != buffered0 || y1 != buffered1) {
            buffered0 = y0;
            buffered1 = y1;
            if (row_done[y0] && y0 != y) { y0 = y1; }
            if (row_done[y1] && y1 != y) { y1 = y0; }
            memcpy(row_buffer, pixelState[y0], screen_w * sizeof(PixelState));
            memcpy(row_buffer2, pixelState[y1], screen_w * sizeof(PixelState));
        }
        int wy = static_cast<int>(fy * 256.0f);

        for (int x = 0; x < screen_w; ++x) {
            float sx = source(x, cx);
            if (sx < -0.5f || sx > screen_w - 0.5f) {
                row[x].clear();
                continue;
            }
            int x0 = std::max(0, static_cast<int>(std::floor(sx)));
            int x1 = std::min(screen_w - 1, x0 + 1);
            float fx = std::max(0.0f, std::min(sx - x0, 1.0f));
            const PixelState& a = row_buffer[x0];
            const PixelState& b = row_buffer[x1];
            const PixelState& c = row_buffer2[x0];
            const PixelState& d = row_buffer2[x1];

            if (a.showsColor() && b.showsColor() && c.showsColor() && d.showsColor()) {
                int wx = static_cast<int>(fx * 256.0f);
                auto lerp = [](int64_t p, int64_t q, int w) { return p + (((q - p) * w) >> 8); };
                int64_t top = lerp(a.position(), b.position(), wx);
                int64_t bottom = lerp(c.position(), d.position(), wx);
                row[x].clear();
                row[x].flags = PixelState::VALID;
                row[x].setPosition(static_cast<uint32_t>(lerp(top, bottom, wy)));
            } else {
                // At the border of the set: nearest neighbor
                row[x] = (fy < 0.5f ? row_buffer : row_buffer2)[fx < 0.5f ? x0 : x1];
                row[x].markIncomplete();
            }
        }
    }
}
