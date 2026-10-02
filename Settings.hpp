#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstdint>
#include "FractalisState.h"

/**
 * Everything that survives a restart: the view and all menu settings. No padding, so two settings can be compared
 * with memcmp.
 */
struct Settings {
    Coordinate center;
    double zoom;
    uint8_t palette;
    uint8_t shading;
    uint8_t supersampling;
    uint8_t color_cycle;
    uint8_t bands;
    uint8_t orbit_trap;
    uint8_t light;
    uint8_t hud;
    uint8_t auto_zoom;
    uint8_t auto_zoom_speed;
    uint8_t auto_zoom_pause;
    uint8_t auto_zoom_full_quality;
    uint8_t reserved[4];
};
static_assert(sizeof(Settings) == 56, "Settings must not contain padding");

/**
 * Stores the settings in the last sectors of the flash. Every save appends a record of one flash page, the newest
 * valid one wins. A sector is only erased when the records wrap around to it, so each sector is erased once every
 * 64 saves: the flash lasts for millions of saves.
 */
namespace settings {

// Loads the newest saved settings. Returns false if there are none (or only ones of an older layout).
bool load(Settings& out);
// Appends the settings to the flash. Stalls both cores for about a millisecond, ~50 ms when a sector gets erased.
bool save(const Settings& settings);

}  // namespace settings

#endif // SETTINGS_H
