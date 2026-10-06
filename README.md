# FractalisPico

A Mandelbrot explorer for the Raspberry Pi Pico 2 (RP2350) with a Pimoroni Pico Display Pack 2.8" (320 x 240, four
buttons, RGB LED).

## Controls

| Button | Press         | Hold        |
|--------|---------------|-------------|
| A      | Menu          | Quick keys  |
| B      | Pan left      | Pan down    |
| X      | Pan right     | Pan up      |
| Y      | Zoom in       | Zoom out    |

Quick keys, while A is held: B resets the view, X shows or hides the info overlay, Y starts or stops auto zoom.
In the menu: X / Y move, A selects (held: second function, e.g. store a view), B goes back (held: closes).
Holding B while powering on starts with the default settings.

## Features

**Exploring**
- Zoom down to x1e72: plain float up to x1e4, then perturbation, every pixel in float relative to a reference orbit
  in 256-bit fixed point, with series approximation. Past x1e32 the pixel offsets get an exponent of their own.
- Both cores calculate, from the center outwards, overclocked to 250 MHz.
- The iteration limit comes from probe points of the view and grows pass by pass, the image refines as it goes.
- Panning only calculates the new parts, zooming keeps the old image as a smooth preview.
- Skips what is known to be in the set: main cardioid and bulbs, minibrots around the reference, periodic orbits.
- Supersampling 2x to 8x, after the view or right away in every pass.

**Colors and light**
- 15 palettes and a solid color picker, auto contrast, bands, color cycling.
- Relief shading with a fixed or rotating light, edge glow.
- Distance estimation with outlines, orbit traps (point, cross, ring, period map, stripes).

**Auto zoom and minibrots**
- Auto zoom dives into the most detailed area on its own, with step size, pause and full quality settings.
- Minibrot finder: finds a minibrot at a chosen depth (x1e10 to x1e70), auto zoom dives there.
- Minibrot glow: marks the minibrots too small to see while you explore. Jump, dive or snap to them, or go
  through them one by one.

**Info overlay**
- Coordinates with as many digits as the zoom needs, zoom, iteration limit.
- Optional: a depth gauge down to the precision limit, where the center is (which bulb, how far from the set) and
  the render time.
- Corner inset: the orbit of the center point or its Julia set.

**Views**
- 10 saved views with a small picture, famous places and deep zoom tests, the way back to the views before.
- All settings and the view survive a restart.
- Plugged into a computer the Pico shows up as a USB drive with the views as text and a picture of the screen.
  Edit a line to go there or to store it in a slot, the drive's README.TXT explains how.

## Build and flash

```
nix develop
./flash_pico.sh            # build and flash
./flash_pico.sh --debug    # with the log on the USB serial port: tio /dev/ttyACM0
```

Without nix: clone with `--recursive` (the Pico SDK and pimoroni-pico are submodules), then
`cmake -B build -G Ninja && ninja -C build` builds `build/FractalisPico.uf2`.

## TODO

- Support the smaller Display Pack 1.14" (240 x 135) again: layout of the menu and the overlay, LED pins.
- Run on the Pico 1 (RP2040) as well, e.g. with the 1.14" pack. It has less RAM (264 KB instead of 520 KB) and no
  single precision FPU, so some features would have to go or shrink (pixel state, reference orbit, USB drive).
