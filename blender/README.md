# Introduction
This folder contains the Blender files and associated exported `.stl` files for the musical box project.

- `music_1.blend` plus associated `musical_box_scale_1.stl`, `musical_box_exercises.stl` and `musical_box_camberwick_green_*.stl`: the Blender file contains objects that help to process the `.svg` files generated over in the [music](../music) directory, see the Python script there for more information on how the `.svg` files are generated.
- `musical_box.blend`/`musical_box.stl`: this is the main Blender file that contains all of the components for the musical box and is described further below.

Throughout the convention is adopted that a component that must be printed multiple times has "xN" on the end, where "N" is the number of times, e.g. `bottom_esp32_mounting_plate_x7`.

# `music_1.blend`
During development, while printing in PLA, the musical belts needed to be printed slowly and be cooled well: the fan had be kept at 100% and no speed greater than 30&nbsp;mm/s was permitted.  Taking the same approach with ASA didn't work so well: though the belt did print it was brittle and delaminated almost immediately.  There may well be a good setting for ASA but I was out of time to experiment so I stuck with PLA: I could always print another belt if the first decayed.

Print `Punched Strip 620 mm, musical_box_camberwick_green_final_0_5` (which means 620&nbsp;mm circumference, based on `musical_box_camberwick_green_final.txt` encoded at half speed) in  PLA at highest resolution with 100% in-fill, no supports or brim required.  Printing a cylinder makes quite a nice relaxing servo noise.

After printing it is worth going over the holes with the hole punch that comes with the plinky-plonky mechanism to make sure they are clean and of good dimensions: to make this easier, I put a dab of white paint on the end of the black bit on the punch that does the cutting, otherwise it is difficult to see where it is when aligning with the holes and you might make things worse.

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

## `plinky_plonky_mount`
All parts should be print at fastest speed, 15% in-fill, in black ASA aside from `plinky_plonky_mount_reflector` which should be printed in white or natural ASA (or it could be painted afterwards I guess).

`plinky_plonky_mount`, being a relatively large/wide ASA part, may need some car to avoid delamination due to warping, or lifting of the print plate.  If you have a print chamber you are fine, if not you may need to get the room that the printer is in up to 40&nbsp;C (I use a space heater).  Supports are required only for the roller-holder arches of `plinky_plonky_mount` and the inset for the screw head in `plinky_plonky_mount_lid`; I allowed automatic support placement and added support blockers for the other regions of `plinky_plonky_mount`: though its cabling tunnels can print a little ropey they are perfectly functional (not visible) and a little ropiness is probably easier than trying to remove support material from cramped spaces.

Note: be careful when removing the support material from the roller-holders or you may snap them off; best to poke the material through from the side.

## `musical_box`
All parts should be printed in black ASA unless, 15% in-fill, unless otherwise stated.

### `bottom`
The `bottom_rack` should be printed on a resin printer in a nice hard resin (e.g. [Siraya Tech Build](https://siraya.tech/pages/build-user-guide)); you will likely have to chop it into two semicircles to make it fit.  Print at fastest speed, no supports or pads required.  The rest of the parts are FDM prints.

`bottom_decoration_bracket_x6`, `bottom_decoration_hex`, `bottom_decoration_slope_x6` and `bottom_decoration_x6` should be printed in natural ASA, fastest speed, 100% in-fill, no supports or brim apart from `bottom_decoration_x6` which will need supports on the build plate.  The remaining parts are printed in black ASA as usual.

The large parts, `bottom_inner` and `bottom_outer` will need supports but may be printed at fastest speed.  The remaining parts will not need supports but should be printed at higher (e.g. 0.1&nbsp;mm) resolution with one exception: `bottom_inner_guide_tool` is not actually part of the musical box, it is just an alignment tool for use during assembly when gluing `bottom_inner_guide` into place, hence it can be printed in anything you like and at highest speed (no supports required).

### `door`
`door_x6` can be printed at fastest speed but will require supports.  When removing from the build plate, be carefuly not to bend them as their right-angledness is important.  You may wish to print an additional `door_x6` to use when test fitting flatted brass bar.

### `sides`
`sides_lifting_plate` needs to be really light and flexible, not to get in the way of the lift's travel.  I tried printing it in black flexible filament but in the end I think this piece probably should not be used directly, just use it as a template for cutting out a hexagon of black paper.  The remaining parts are printed in black ASA and PLA.

`sides_panel_outer` and `sides_panel_inner` I printed in PLA rather than ASA: these are rather thin and, like with the [music belts](../music) I couldn't make ASA behave; it would print but be slightly too "wiggly" in the vertical dimension to fit well on the musical box once printed.  The outer will be covered in the decoration of the musical box and the inner will be inside under the doors, so neither should be too exposed to sunlight.  `sides_panel_outer` can be printed at fastest speed, 100% in-fill, no supports or brim required, however it is quite important that `sides_panel_inner`, though tall and thin, does not come out with an warps/wiggles in it (or it may foul the lift or `sides_lifting_plate`) hence I printed that piece with the same settings as `music_1.blend` above to achieve a higher quality result.  The remaining parts are printed in black ASA.

`sides_column_x6` and `sides_brace` can be printed at fastest speed, the rest at a higher resolution if you can, but it is not critical.  You may wish to print an additional `sides_stepper_spindle_adapter_x6` to use when test fitting flatted brass bar.  None of the parts require supports.

### `top`
The parts `top_decoration_x6`, `top_decoration_slope_x6` and `top_decoration_bracket_x6` should be printed in natural ASA, fastest speed, 100% in-fill, no supports or brim aside from `top_decoration_x6`, which will need supports on the build plate.  The remaining parts are printed in black ASA as usual.

The large part, `top`, should be printed at fastest speed, the rest at higher (e.g. 0.1&nbsp;mm) resolution if you can for a nicer finish.

### `stand`
The `stand_gear_*` parts should be printed on a resin printer in a nice hard resin (e.g. [Siraya Tech Build](https://siraya.tech/pages/build-user-guide)) at fastest speed, no supports or pads should be required if you are careful.  The rest of the parts are FDM prints.

The large parts, `stand_lower`, `stand_upper`, `stand_lid`, `stand_motor_extension` and `stand_motor_extension_cover` should be printed at fastest speed.  All bar `stand_motor_extension_cover` will require supports, but where possible it is worth preventing supports on holes smaller than 3&nbsp;mm as it is not really necessary there and supports in tiny holes can be difficult to remove.  Removing the supports from `stand_upper` is a right-royal pain: they are so thin that the support tends to crumble rather than pull away.  There is no need to remove support material from the lettering, since that is only cosmetic and you can still read the letters, but for around the edge I found the best technique was to get a knife under the support material and slide it around.

The rest of the parts should be printed in higher (e.g. 0.1&nbsp;mm) resolution; only `stand_knob_and_stepper_spindle_adapter`, and possibly `stand_ftdi_mounting_plate_top` (depending how good your printer is at overhangs) will require supports.

Note: to build the `stand` you will also need the seventh of those `bottom_esp32_mounting_plate_x7` and `bottom_tmc2209_mounting_plate_x7` parts from `bottom`.