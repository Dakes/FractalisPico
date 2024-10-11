# FractalisPico

This is a Mandelbrot Fractal renderer and explorer for the rasbberry pi pico (2) and pimoroni display pack (1.14" with 4 Buttons).  
It should easily work with the bigger pimoroni pico display pack 2.0  

It's features are:
- zooming and panning
- on Pan only re-renders the new parts, instead of the whole frame
- greater zoom depth by the use of DoubleDouble. (Dynamically switches to it from native double, once the max depth for double is reached)
- dis-/enable UI
- Automatic zoom
- Optimizations, to skip the calculation for the main cardioid and secondary bulb
- pre-renders a frame at a lower iteration count, before refining the image. (Only up to a certain depth)
- dynamic iteration level. (Higher the deeper you go capped at a max value)

On the pico 2 it is about 10x faster than on the pico 1, because it has native float registers.  

## TODO
- optimize the color rendering: normalize the difference in iteration count to cycle through the hue wheel more strongly. Right now contrast can be pretty low in certain areas
- add shading using the brightness
- save current coordinates
- Coordinates can't be displayed beyond a certain precision
