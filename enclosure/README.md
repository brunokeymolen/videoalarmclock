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
printer runs tight, scale the cutout rather than forcing the board.

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
