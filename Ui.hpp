#ifndef UI_H
#define UI_H

#include "AutoZoom.hpp"
#include "FractalisState.h"
#include "Settings.hpp"
#include "fractalis.h"
#include "palette.h"
#include <cstddef>
#include <cstdint>

namespace pimoroni { class PicoGraphics; }

// Defined by the main program
extern FractalisState state;
extern Fractalis fractalis;
extern AutoZoom autoZoom;
extern palette::Palette color_palette;
uint32_t now_ms();
// Shows a color on the LED for a moment
void show_led_feedback(uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms);
// For the statistics page
uint32_t system_clock_khz();
unsigned free_ram();

/**
 * What is shown on top of the image and changed with the buttons: the info overlay, the menu (see menu.hpp) and the
 * quick functions.
 */
namespace ui {

// What the button interrupt makes of the presses, see sample_buttons()
enum class Input : uint8_t {
    PRESS, LONG, REPEAT,  // B, X, Y: tap, long press, repeated while held
    QUICK,                // B, X or Y pressed while A is held
    OPEN,                 // A tapped: opens the menu
    // In the menu
    UP, DOWN, OK, HOLD, BACK, CLOSE,
};

// Set by the button interrupt when A opens the menu, cleared when the menu closes. While it is set, the
// interrupt sends the inputs of the menu.
extern volatile bool menu_active;
// A is held: the quick functions of B, X and Y are shown
extern volatile bool quick_shown;

// Drawing of the current view, counted by the main loop
struct FrameStats {
    uint32_t frames;
    uint64_t drawing_us;
};
extern FrameStats view_frames;

// Loads the saved settings (defaults: not them, only the saved views) and sets up the menu
void start(bool defaults);
void handle(Input input, int button);
/**
 * Call every loop: timers (auto hide, menu timeout, saving the settings) and the animation of the colors and the
 * light. Returns true while something animates.
 */
bool update(uint32_t now, uint32_t elapsed_ms);
// Something is drawn on top of the image
bool overlay_wanted();
// LED color for the current state
void led_color(uint8_t& r, uint8_t& g, uint8_t& b);
// Before drawing a frame
void prepare_frame(pimoroni::PicoGraphics& g);
// Draws the overlay into a strip of the image (display byte order)
void draw_strip(pimoroni::PicoGraphics& g, uint16_t* strip, int first_row, int rows);
/**
 * Work for core0 instead of helping to calculate the image: the minibrot search, a slice of it. Returns false if
 * there is none. interrupt: stops the slice early (a button was pressed).
 */
bool work(bool (*interrupt)());
// A minibrot search is running
bool searching();

/**
 * Prints a coordinate with the given number of decimals (cut off, not rounded), exactly: printf only knows double,
 * which is not enough for deep zooms. Starts a new line, indented, every line_length characters.
 */
void format_coordinate(const Fixed& value, int decimals, int line_length, char* out, size_t length);

}  // namespace ui

#endif // UI_H
