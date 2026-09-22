# The stand

A base that holds the board at a 15° tilt, for a bedside table.

| File | |
| --- | --- |
| `base-esp32-clock.stl` | print this |
| `base-esp32-clock.scad` | the OpenSCAD source it came from |

## Printing

Nothing special: PLA, 0.2 mm layers, 15–20% infill. It prints flat on
its base with no supports.

The board slides into the slot. The fit is deliberately close — if your
printer runs tight, increase the slot dimensions in the OpenSCAD source.

## Changing it

The dimensions are named at the top of the `.scad`:

```
length1, width1, height1   the base block
lengthEsp, widthEsp, heightEsp   the slot cut out of it
```

The tilt is the `rotate([15, 0, 0])` near the bottom. The comment on the
translate above it notes that a `-8` variant was also built — that
number sets how deep the board sits.

Render with [OpenSCAD](https://openscad.org/):

```sh
openscad -o base-esp32-clock.stl base-esp32-clock.scad
```

## Origin & Attribution

This is an independently created OpenSCAD stand for the Waveshare ESP32-P4 Smart 86 Touch Display.

I originally printed and tested [https://makerworld.com/en/models/2666288-waveshare-esp32-p4-smart-86-touch-display-stand#profileId-2950356], but the fit was too tight for my display / Anycubic i3 Mega printer tolerances. 
I therefore recreated the stand from scratch in OpenSCAD using my own dimensions.

The slot is slightly wider and deeper, and the rounding and some dimensions differ from the referenced model. 

The original MakerWorld model inspired the general concept. If your printer and display tolerances differ from mine, the MakerWorld model may work perfectly well for you.
[https://makerworld.com/en/models/2666288-waveshare-esp32-p4-smart-86-touch-display-stand#profileId-2950356]

Included:

OpenSCAD source & STL ready for printing


