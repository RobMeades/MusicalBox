# Introduction
This folder contains the Blender files and associated exported `.stl` files for the musical box project.

- `music_1.blend` plus associated `musical_box_scale_1.stl`, `musical_box_exercises.stl` and `musical_box_camberwick_green_*.stl`: the Blender file contains objects that help to process the `.svg` files generated over in the [music](../music) directory, see there for more information.
- `musical_box.blend`/`musical_box.stl`: this is the main Blender file that contains all of the components for the musical box and is described further below.

Throughout the convention is adopted that a component that must be printed multiple times has "xN" on the end, where "N" is the number of times, e.g. `esp32_mounting_plate_1_x7`.

# `musical_box.blend`
There are three main components in `musical_box.blend`:

- `lifting_table`: this is a modification/butchering of [Michael Burger](https://www.thingiverse.com/Mike0365/designs)'s marvellous [lifting_table](https://www.thingiverse.com/thing:5429165) from [Thingiverse](https://www.thingiverse.com/); all rights remain with him, I have merely taken his good work and adapted it to fit inside the musical box to form the lifting mechanism.  Changes are:
  - make it slightly taller,
  - motorise it,
  - add sensors to allow automation,
  - create an indent into which a magnet can be glued to hold whatever is placed on top of the lift,
  - add a hole in the base through which cables can be routed from below.
  See [here](https://www.meades.org/misc/musical_box/musical_box.html#Lift) for more information on how this was done.
- `plinky_plonky`: a motorised [plinky-plonky](https://www.youtube.com/watch?v=XHdWuA18UNk), 'cos a musical box needs one of those.
- `musical_box`: the main event, the Camberwick Green musical box made real.

Below find printing instructions for all three of these components.  All musical box parts are printed on an FDM printer in black ASA (ASA for UV safety) unless otherwise stated.

## 'lifting_table`
Print at 0.1&nbsp;mm layer height in black ASA aside from parts 13 and 14 which should be printed in natural ASA, a bright colour for the optical sensors; that or paint those part afterwards.  In Michael Burger's original no supports were required but the modification have meant that parts 2, 7, 8 and 11 do now require supports on the build plate.

## `plinky_plonky`
TBC.

## `musical_box`
All parts should be printed in black ASA unless, 15% in-fill, unless otherwise stated.

### `bottom`
TBC.

### `door`
TBC.

### `sides`
TBC.

### `top`
TBC.

### `stand`
The `gear_*` parts should be printed on a resin printer in a nice hard resin (e.g. [Siraya Tech Build](https://siraya.tech/pages/build-user-guide)) at fastest speed, no supports or pads should be required if you are careful.  The rest of the parts are FDM prints.

The large parts, `stand_lower`, `stand_upper`, `lid`, `motor_extension` and `motor_extension_cover` should be printed at fastest speed.  All bar `lid` and `motor_extension_cover` will require supports, but where possible it is worth preventing supports on holes smaller than 3&nbsp;mm as it is not really necessary and that support material can be difficult to remove.

The rest of the parts should be printed in higher (e.g. 0.1 mm) resolution; only `knob_and_stepper_spindle_adapter` will require supports.

Note: to build the `stand` you will also need the seventh of those `esp32_mounting_plate_1_x7` and `tmc2209_mounting_plate_1_x7` parts from `bottom`.