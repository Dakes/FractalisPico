#include "palette.h"
#include "globals.h"
#include <algorithm>
#include <cmath>

namespace palette {

namespace {

constexpr ColorStop ULTRA[] = {
    {0.0000f,   0,   7, 100},
    {0.1600f,  32, 107, 203},
    {0.4200f, 237, 255, 255},
    {0.6425f, 255, 170,   0},
    {0.8575f,   0,   2,   0},
};

constexpr ColorStop FIRE[] = {
    {0.00f,  10,   0,  20},
    {0.25f, 150,  10,  30},
    {0.50f, 255, 120,   0},
    {0.70f, 255, 230, 120},
    {0.85f, 120,  40,  20},
};

constexpr ColorStop OCEAN[] = {
    {0.00f,   0,  10,  40},
    {0.30f,   0,  90, 140},
    {0.55f,  60, 220, 210},
    {0.70f, 240, 255, 250},
    {0.85f,  20,  60, 110},
};

constexpr ColorStop NEON[] = {
    {0.00f,  20,   0,  60},
    {0.25f, 160,   0, 200},
    {0.50f, 255,  40, 140},
    {0.70f, 255, 200,  60},
    {0.85f,   0, 200, 255},
};

constexpr ColorStop PASTEL[] = {
    {0.00f, 250, 190, 210},
    {0.18f, 255, 214, 170},
    {0.36f, 250, 245, 175},
    {0.54f, 180, 235, 200},
    {0.72f, 170, 210, 250},
    {0.88f, 210, 185, 245},
};

constexpr ColorStop LIME[] = {
    {0.00f,   5,  25,   5},
    {0.35f,  60, 160,  20},
    {0.60f, 190, 255,  40},
    {0.75f, 245, 255, 190},
};

// Two colors only
constexpr ColorStop LIME_PURPLE[] = {
    {0.00f,  70,   0, 110},
    {0.50f, 180, 255,  30},
};

constexpr ColorStop ICE[] = {
    {0.00f,   0,  15,  50},
    {0.40f,  40, 170, 230},
    {0.70f, 235, 250, 255},
};

constexpr ColorStop GOLD[] = {
    {0.00f,  35,  15,   0},
    {0.45f, 230, 160,  20},
    {0.70f, 255, 240, 180},
};

constexpr ColorStop SILVER[] = {
    {0.00f,  10,  10,  12},
    {0.50f, 235, 235, 240},
};

constexpr ColorStop MAGMA[] = {
    {0.00f,   0,   0,   5},
    {0.20f,  60,  15, 110},
    {0.40f, 180,  50, 120},
    {0.60f, 250, 120,  80},
    {0.78f, 252, 250, 190},
};

constexpr ColorStop SUNSET[] = {
    {0.00f,  30,  10,  70},
    {0.25f, 150,  30, 120},
    {0.50f, 250,  90,  70},
    {0.72f, 255, 200,  90},
};

constexpr ColorStop AURORA[] = {
    {0.00f,   0,  10,  30},
    {0.30f,  20, 200, 120},
    {0.55f, 120, 255, 200},
    {0.75f, 140,  60, 200},
};

constexpr ColorStop SEPIA[] = {
    {0.00f,  25,  15,   5},
    {0.45f, 160, 110,  60},
    {0.70f, 245, 225, 190},
};

// New palettes go to the end: settings and view slots store the index
constexpr Definition PALETTES[] = {
    {"Classic", nullptr, 0, 1.0f, START_HUE},  // the original HSV rainbow
    {"Ultra", ULTRA, sizeof(ULTRA) / sizeof(ULTRA[0]), 1.0f, 0.0f},
    {"Fire", FIRE, sizeof(FIRE) / sizeof(FIRE[0]), 1.0f, 0.0f},
    {"Ocean", OCEAN, sizeof(OCEAN) / sizeof(OCEAN[0]), 1.0f, 0.0f},
    {"Neon", NEON, sizeof(NEON) / sizeof(NEON[0]), 1.0f, 0.0f},
    {"Pastel", PASTEL, sizeof(PASTEL) / sizeof(PASTEL[0]), 1.0f, 0.0f},
    {"Lime", LIME, sizeof(LIME) / sizeof(LIME[0]), 1.0f, 0.0f},
    {"Lime Purple", LIME_PURPLE, sizeof(LIME_PURPLE) / sizeof(LIME_PURPLE[0]), 1.0f, 0.0f},
    {"Ice", ICE, sizeof(ICE) / sizeof(ICE[0]), 1.0f, 0.0f},
    {"Gold", GOLD, sizeof(GOLD) / sizeof(GOLD[0]), 1.0f, 0.0f},
    {"Silver", SILVER, sizeof(SILVER) / sizeof(SILVER[0]), 1.0f, 0.0f},
    {"Magma", MAGMA, sizeof(MAGMA) / sizeof(MAGMA[0]), 1.0f, 0.0f},
    {"Sunset", SUNSET, sizeof(SUNSET) / sizeof(SUNSET[0]), 1.0f, 0.0f},
    {"Aurora", AURORA, sizeof(AURORA) / sizeof(AURORA[0]), 1.0f, 0.0f},
    {"Sepia", SEPIA, sizeof(SEPIA) / sizeof(SEPIA[0]), 1.0f, 0.0f},
};
constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

// Auto contrast: number of palette cycles between the 1st and 99th percentile of the values on screen
constexpr float CYCLES_PER_SCREEN = 2.0f;
constexpr float MIN_CYCLES = 0.5f;   // per unit of t
constexpr float MAX_CYCLES = 400.0f;
constexpr float TRANSITION_SPEED = 0.2f;  // fraction per frame

// Far away from the set the colors fade in from black
constexpr uint32_t FADE_IN_END = static_cast<uint32_t>(VALUE_THRESHOLD * (1 << POSITION_BITS));

// Relief shading
constexpr float LIGHT_HEIGHT = 1.2f;  // higher = softer shading
constexpr float AMBIENT = 0.25f;
// Slopes up to this (squared, in palette positions per pixel) count as flat: a step of the last bit would otherwise
// show up as a line in otherwise flat areas
constexpr float MIN_SLOPE_SQ = 2.0f;

uint16_t pack565(float r, float g, float b) {
    auto c = [](float v) { return static_cast<uint16_t>(std::max(0.0f, std::min(v, 255.0f))); };
    return static_cast<uint16_t>(((c(r) & 0xF8) << 8) | ((c(g) & 0xFC) << 3) | (c(b) >> 3));
}

uint16_t hsv(float h, float s, float v) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    v *= 255.0f;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0: return pack565(v, t, p);
        case 1: return pack565(q, v, p);
        case 2: return pack565(p, v, t);
        case 3: return pack565(p, q, v);
        case 4: return pack565(t, p, v);
        default: return pack565(v, p, q);
    }
}

// Smooth (cosine) interpolation between cyclic color stops
uint16_t gradient(const Definition& def, float x) {
    x = x - std::floor(x);
    int n = def.stop_count;
    int next = 0;
    while (next < n && def.stops[next].position <= x) next++;
    const ColorStop& a = def.stops[(next + n - 1) % n];
    const ColorStop& b = def.stops[next % n];
    float start = a.position;
    float end = b.position;
    if (end <= start) end += 1.0f;
    float xx = x < start ? x + 1.0f : x;
    float f = (xx - start) / (end - start);
    f = 0.5f - 0.5f * std::cos(f * 3.14159265f);
    return pack565(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f);
}

}  // namespace

int count() {
    return PALETTE_COUNT;
}

const char* name(int index) {
    return PALETTES[((index % PALETTE_COUNT) + PALETTE_COUNT) % PALETTE_COUNT].name;
}

uint16_t sample(int index, float x) {
    const Definition& def = PALETTES[((index % PALETTE_COUNT) + PALETTE_COUNT) % PALETTE_COUNT];
    x += def.offset;
    return def.stop_count == 0 ? hsv(x, 1.0f, 1.0f) : gradient(def, x);
}

uint32_t position(float smooth_iteration) {
    float t = std::log(1.0f + std::max(smooth_iteration, 0.0f)) / 2.0f;
    float pos = t * static_cast<float>(1 << POSITION_BITS);
    return static_cast<uint32_t>(std::min(pos, 16777215.0f));
}

uint32_t trap_position(float distance_sq) {
    // -log2(distance): every halving of the distance is another 2^18 positions, 2^16 to 2^-48 fit into 24 bit
    float octaves = -0.5f * std::log2(std::max(distance_sq, 1e-30f)) + 16.0f;
    return static_cast<uint32_t>(std::max(0.0f, std::min(octaves * 262144.0f, 16777215.0f)));
}

uint8_t spread_level(uint32_t range) {
    if (range < 2) return 1;
    // nearest level on the log scale, level k stands for 8^(k - 1)
    int level = static_cast<int>(std::lround((std::log2(static_cast<float>(range)) + 3.0f) / 3.0f));
    return static_cast<uint8_t>(std::max(1, std::min(level, 7)));
}

uint32_t spread_width(uint8_t level) {
    // Twice the level's range: averaging a bit wider matches the average of the sub-sample colors best
    return level <= 1 ? 0 : 1u << (3 * level - 2);
}

Palette::Palette()
    : current(0), phase_offset(0), range_start(0.0f), cycles(1.0f), target_range_start(0.0f), target_cycles(1.0f),
      lut_step(0) {
    select(0);
}

void Palette::set_light(float angle) {
    light_x = std::cos(angle);
    light_y = std::sin(angle);
}

void Palette::set_bands(float bands) {
    this->bands = bands;
    update_lut_step();
}

void Palette::update_lut_step() {
    float c = cycles * PALETTES[current].cycles * bands;
    lut_step = static_cast<uint32_t>(c * LUT_SIZE * 65536.0f / (1 << POSITION_BITS));
}

void Palette::analyze(PixelState* const* pixels, int width, int height) {
    uint32_t lowest = UINT32_MAX, highest = 0;
    int count = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const PixelState& p = pixels[y][x];
            if (!p.hasPosition()) continue;
            uint32_t pos = p.position();
            lowest = std::min(lowest, pos);
            highest = std::max(highest, pos);
            count++;
        }
    }
    if (count < 16 || highest <= lowest) return;

    // Percentiles from a histogram, so a few extreme pixels don't dominate
    constexpr int BINS = 256;
    uint32_t histogram[BINS] = {};
    float bin_size = static_cast<float>(highest - lowest + 1) / BINS;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const PixelState& p = pixels[y][x];
            if (!p.hasPosition()) continue;
            int bin = static_cast<int>((p.position() - lowest) / bin_size);
            histogram[std::min(bin, BINS - 1)]++;
        }
    }
    int low_bin = 0, high_bin = BINS - 1;
    for (int sum = 0; low_bin < BINS - 1 && (sum += histogram[low_bin]) < count / 100; ++low_bin) {}
    for (int sum = 0; high_bin > 0 && (sum += histogram[high_bin]) < count / 100; --high_bin) {}
    float low = lowest + low_bin * bin_size;
    float high = lowest + (high_bin + 1) * bin_size;

    float range_t = (high - low) / (1 << POSITION_BITS);
    target_range_start = low;
    target_cycles = std::max(MIN_CYCLES, std::min(CYCLES_PER_SCREEN / std::max(range_t, 1e-6f), MAX_CYCLES));
}

bool Palette::animate() {
    bool changing = false;
    float scale = (1 << POSITION_BITS) / std::max(cycles, target_cycles);
    if (std::fabs(target_range_start - range_start) > scale * 0.002f) {
        range_start += (target_range_start - range_start) * TRANSITION_SPEED;
        changing = true;
    } else {
        range_start = target_range_start;
    }
    float ratio = target_cycles / cycles;
    if (std::fabs(ratio - 1.0f) > 0.002f) {
        cycles *= std::pow(ratio, TRANSITION_SPEED);
        changing = true;
    } else {
        cycles = target_cycles;
    }
    update_lut_step();
    return changing;
}

void Palette::select(int index) {
    current = ((index % PALETTE_COUNT) + PALETTE_COUNT) % PALETTE_COUNT;
    const Definition& def = PALETTES[current];
    for (int i = 0; i < LUT_SIZE; ++i) {
        float x = static_cast<float>(i) / LUT_SIZE + def.offset;
        lut[i] = def.stop_count == 0 ? hsv(x, 1.0f, 1.0f) : gradient(def, x);
    }
    prefix[0][0] = prefix[1][0] = prefix[2][0] = 0;
    for (int i = 0; i < LUT_SIZE; ++i) {
        prefix[0][i + 1] = static_cast<uint16_t>(prefix[0][i] + (lut[i] >> 11));
        prefix[1][i + 1] = static_cast<uint16_t>(prefix[1][i] + ((lut[i] >> 5) & 0x3F));
        prefix[2][i + 1] = static_cast<uint16_t>(prefix[2][i] + (lut[i] & 0x1F));
    }
    update_lut_step();
}

void Palette::next() {
    select(current + 1);
}

const char* Palette::name() const {
    return palette::name(current);
}

void Palette::set_phase(float phase) {
    phase = phase - std::floor(phase);
    phase_offset = static_cast<uint32_t>(phase * LUT_SIZE * 65536.0f);
}

uint16_t Palette::average(uint32_t start, uint32_t width) const {
    if (width >= LUT_SIZE) {
        start = 0;
        width = LUT_SIZE;
    }
    start &= LUT_SIZE - 1;
    uint32_t end = start + width;
    uint32_t channels[3];
    for (int c = 0; c < 3; ++c) {
        const uint16_t* sums = prefix[c];
        uint32_t sum = end <= LUT_SIZE ? sums[end] - sums[start]
                                       : sums[LUT_SIZE] - sums[start] + sums[end - LUT_SIZE];
        channels[c] = (sum + width / 2) / width;
    }
    return static_cast<uint16_t>((channels[0] << 11) | (channels[1] << 5) | channels[2]);
}

void Palette::render(PixelState* const* pixels, int width, int height, uint16_t* frame_buffer) const {
    render_rows(pixels, width, height, 0, height, frame_buffer);
}

// The value a pixel is drawn with: its own, or blended from the calculated points around it if it isn't calculated
// yet. The image gets sharper with every finer grid (see Fractalis::next_in_order()).
static PixelState displayed(PixelState* const* pixels, int width, int height, int x, int y,
                            const ScreenRect* content) {
    const PixelState& own = pixels[y][x];
    if (own.isValid()) return own;
    PixelState p = own;
    uint32_t position;
    Blend blend = interpolated_position(pixels, width, height, x, y, position);
    if (blend == Blend::COLOR) {
        p.setEscaped(position);
    } else if (blend == Blend::NONE) {
        bool uncovered = content && !content->empty()
                         && (x < content->x0 || x >= content->x1 || y < content->y0 || y >= content->y1);
        const PixelState* near = uncovered ? nullptr : nearest_calculated(pixels, width, height, x, y, false);
        if (near) {
            p = *near;
        } else if (uncovered) {
            // Uncovered by a pan or zoom out and nothing calculated nearby yet: the edge of the old content,
            // blended between points every EDGE_STEP pixels, so it continues softly instead of in stripes
            constexpr int EDGE_STEP = 16;
            int gx = (x / EDGE_STEP) * EDGE_STEP, gy = (y / EDGE_STEP) * EDGE_STEP;
            float fx = static_cast<float>(x - gx) / EDGE_STEP, fy = static_cast<float>(y - gy) / EDGE_STEP;
            float sum = 0, weight_sum = 0;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    int ex = std::max(content->x0, std::min(gx + i * EDGE_STEP, content->x1 - 1));
                    int ey = std::max(content->y0, std::min(gy + j * EDGE_STEP, content->y1 - 1));
                    const PixelState& edge = pixels[ey][ex];
                    if (!edge.showsColor()) continue;
                    float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    sum += w * static_cast<float>(edge.position());
                    weight_sum += w;
                }
            }
            if (weight_sum > 0) p.setEscaped(static_cast<uint32_t>(sum / weight_sum));
        }
    }
    return p;
}

void Palette::render_rows(PixelState* const* pixels, int width, int height, int first_row, int rows,
                          uint16_t* out_rows, const ScreenRect* content) const {
    // Static: too big for the stack, and only core0 draws the frames
    static PixelState rows_buffer[2 * MAX_WIDTH];
    render_rows(pixels, width, height, first_row, rows, out_rows, content, rows_buffer);
}

void Palette::render_rows(PixelState* const* pixels, int width, int height, int first_row, int rows,
                          uint16_t* out_rows, const ScreenRect* content, PixelState* scratch) const {
    const int64_t start = static_cast<int64_t>(range_start);
    if (width > MAX_WIDTH) return;
    // The displayed values of the row and the one below, for the shading
    PixelState* row = scratch;
    PixelState* below = scratch + width;
    auto fill = [&](PixelState* out, int y) {
        for (int x = 0; x < width; ++x) out[x] = displayed(pixels, width, height, x, y, content);
    };
    fill(row, first_row);
    for (int y = first_row; y < first_row + rows; ++y) {
        fill(below, y + 1 < height ? y + 1 : y - 1);
        float dir_y = y + 1 < height ? 1.0f : -1.0f;
        uint16_t* out = out_rows + (y - first_row) * width;

        for (int x = 0; x < width; ++x) {
            const PixelState& p = row[x];
            if (!p.showsColor()) {
                out[x] = 0;
                continue;
            }
            uint32_t pos = p.position();
            int64_t relative = static_cast<int64_t>(pos) - start;
            uint32_t index = static_cast<uint32_t>((relative * lut_step + phase_offset) >> 16);
            uint16_t c = lut[index & (LUT_SIZE - 1)];
            float brightness = pos < FADE_IN_END ? static_cast<float>(pos) / FADE_IN_END : 1.0f;

            // Supersampled: the average color over the range of palette positions the sub-samples covered, and
            // darker by the share of them that is in the set
            uint8_t spread = p.spread();
            if (spread >= 2) {
                uint32_t width = spread >= 7 ? LUT_SIZE
                               : static_cast<uint32_t>((static_cast<uint64_t>(spread_width(spread)) * lut_step) >> 16);
                if (width >= 2) {
                    c = average(index - width / 2, width);
                }
            }
            brightness *= 1.0f - 0.25f * p.coverage();

            if (shading) {
                // Slope of the smooth iteration count towards the right and bottom neighbor.
                // Only its direction matters, which makes the relief look the same at every zoom level.
                const PixelState& right = row[x + 1 < width ? x + 1 : x - 1];
                float dir_x = x + 1 < width ? 1.0f : -1.0f;
                float gx = right.showsColor() ? dir_x * (static_cast<float>(right.position()) - pos) : 0.0f;
                float gy = below[x].showsColor() ? dir_y * (static_cast<float>(below[x].position()) - pos) : 0.0f;
                float length_sq = gx * gx + gy * gy;
                // The palette position is the height. A slope faces the light when it rises away from it, so it is
                // lit when the gradient points away from the light. Flat areas are lit like a horizontal surface.
                float d = length_sq > MIN_SLOPE_SQ ? -(gx * light_x + gy * light_y) / std::sqrt(length_sq) : 0.0f;
                float light = (d + LIGHT_HEIGHT) / (1.0f + LIGHT_HEIGHT);
                brightness *= AMBIENT + (1.0f - AMBIENT) * light;
            }

            if (brightness < 0.999f) {
                uint32_t scale = static_cast<uint32_t>(brightness * 256.0f);
                uint32_t r = ((c >> 11) * scale) >> 8;
                uint32_t g = (((c >> 5) & 0x3F) * scale) >> 8;
                uint32_t b = ((c & 0x1F) * scale) >> 8;
                c = static_cast<uint16_t>((r << 11) | (g << 5) | b);
            }
            // The display expects big endian
            out[x] = static_cast<uint16_t>((c >> 8) | (c << 8));
        }
        std::swap(row, below);
    }
}

}  // namespace palette
