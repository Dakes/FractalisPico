#ifndef GLOBALS_H
#define GLOBALS_H

#define DEBUG true
// System clock. The RP2350 is specified for 150 MHz, but runs fine a good bit faster. 0 = don't change
#define SYS_CLOCK_KHZ 200000

#define UPDATE_SLEEP 16
#define FRAME_INTERVAL_MS 100    // Time between display refreshes while calculating
#define ANIMATION_INTERVAL_MS 33 // Time between display refreshes during color animations
#define CORE0_WORK_MS 20         // How long core0 helps calculating before it checks input again
#define COLOR_CYCLE_SPEED 0.04f  // Palette cycles per second during auto zoom. 0 = off

// Wait this long for a serial terminal at startup when DEBUG is enabled
#define USB_WAIT_MS 1000

// The settings and the view are saved to the flash this long after the last button press, if anything changed
#define SAVE_DELAY_MS 5000
// During auto zoom the view is saved at most this often
#define AUTO_ZOOM_SAVE_INTERVAL_MS 60000

// Button timings
#define BUTTON_SAMPLE_MS 2
#define BUTTON_DEBOUNCE_SAMPLES 4
#define LONG_PRESS_MS 350
#define DOUBLE_TAP_MS 300        // A tap followed by a press within this time selects the second function layer
#define REPEAT_MS 250            // Repeat interval of long press actions while the button is held

#define PAN_CONSTANT 0.1
#define ZOOM_CONSTANT 0.1

#define MAX_ITER 10000

// Precision switches. Float is by far the fastest on the RP2350 (single precision FPU), double is a good bit
// slower (DCP coprocessor), DoubleDouble needs many double operations per step.
#ifndef FLOAT_MAX_ZOOM
#define FLOAT_MAX_ZOOM 1e4
#endif
#ifndef DOUBLE_MAX_ZOOM
#define DOUBLE_MAX_ZOOM 5e11
#endif
// Beyond this even DoubleDouble runs out of precision (~32 digits) and pixels become blocky
#define DOUBLE_DOUBLE_MAX_ZOOM 1e31
// From here up to DOUBLE_DOUBLE_MAX_ZOOM pixels are calculated with perturbation: in single precision relative to
// a double-double reference orbit. Between FLOAT_MAX_ZOOM and this, pixels are calculated in double.
#ifndef PERTURBATION_MIN_ZOOM
#define PERTURBATION_MIN_ZOOM FLOAT_MAX_ZOOM
#endif
// Above this zoom level the main cardioid/bulb check and periodicity checks are not precise enough
#define OPTIMIZATIONS_MAX_ZOOM 1e7
// Every calculation starts with a low iteration limit, which doubles with every pass
#define FIRST_PASS_ITER 32
// After reaching the normal limit, keep doubling it while a pass still resolves at least this many pixels
#ifndef REFINE_MIN_PIXELS
#define REFINE_MIN_PIXELS 100
#endif
// ... up to this factor of the normal limit
#define REFINE_MAX_FACTOR 8
// Squared escape radius. A large radius makes the smooth coloring smoother.
#define BAILOUT_SQ 1048576.0

#define START_HUE 0.6222
// Far away from the set the colors fade in from black
#define VALUE_THRESHOLD 0.06f

#endif // GLOBALS_H
