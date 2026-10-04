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
    uint8_t bands;  // id of the band count, see the menu
    uint8_t orbit_trap;
    uint8_t light;
    uint8_t hud;    // info overlay: 0 off, 1 on, 2 auto
    uint8_t auto_zoom;
    uint8_t auto_zoom_speed;
    uint8_t auto_zoom_pause;
    uint8_t auto_zoom_full_quality;
    // Added later, 0 is the default (older records have 0 there)
    uint8_t show_probes;
    uint8_t supersample_right_away;
    uint8_t set_display_preview;  // 0: undecided pixels are part of the set, 1: they keep the preview color
    uint8_t color_cycle_speed;    // option of the menu + 1, 0 = the default
    // Saved views of older versions (slots 1 and 2), zoom 0 = empty. They show until these slots are stored again,
    // the views are stored in their own sectors now (see settings::view()).
    struct Slot {
        Coordinate center;
        double zoom;
    } slots[2];
};
static_assert(sizeof(Settings) == 232, "Settings must not contain padding");

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

/**
 * Saved views, each with a small picture of it. Every slot has two sectors of its own in front of the settings.
 */
constexpr int VIEW_SLOTS = 10;
// The picture: 1/5 of the screen, RGB565 in display byte order
constexpr int THUMBNAIL_W = 64;
constexpr int THUMBNAIL_H = 48;

struct View {
    Coordinate center;
    double zoom;
    // The look it was stored with, restored when going there
    uint8_t has_look;
    uint8_t palette, bands, orbit_trap, shading, light;
    uint8_t reserved[2];
};
static_assert(sizeof(View) == 80, "View must not contain padding");

// The view stored in the slot, nullptr if it is empty
const View* view(int slot);
// The picture of the view in the slot, nullptr if there is none
const uint16_t* thumbnail(int slot);
/**
 * Stores a view. draw_thumbnail draws its picture (THUMBNAIL_W x THUMBNAIL_H) into the buffer, nullptr: none.
 * Erases two sectors: stalls both cores for ~100 ms.
 */
bool store_view(int slot, const View& view, void (*draw_thumbnail)(uint16_t* out));
// Empties the slot, stalls both cores like store_view()
bool clear_view(int slot);

}  // namespace settings

#endif // SETTINGS_H
