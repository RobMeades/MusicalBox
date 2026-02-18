#!/usr/bin/env python

# Copyright 2026 Rob Meades
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This python script generates an SVG image of a punched strip suitable for
# playing in one of those plinky-plonky thingies, e.g.:
#
# https://www.ebay.co.uk/itm/397246486151
#
# ...visible here:
#
# https://youtu.be/XHdWuA18UNk
#

import sys
import argparse
import os

# List of valid notes
NOTE_STRING_LIST = ["c0", "d0", "e0", "f0", "g0", "a0", "b0", "c1", "d1", "e1", "f1", "g1", "a1", "b1", "c2"]

# The number of millimetres per SVG pixel by default
MILLIMETRES_PER_PIXEL_DEFAULT = 2

# Scaled factor for the horizontal distance between notes
SPEED_SCALE_DEFAULT = 1

# The horizontal distance between quaver beats on the original paper strip
QUAVER_DISTANCE_MM = 4

# The vertical distance between notes on the original paper strip
NOTE_STEP_MM = 2

# The distance between the last or first notes and the bottom or top of the original paper strip
MARGIN_TOP_BOTTOM_MM = 6.5

# The lead-in distance on the front of the original paper strip
LEAD_IN_MM = 30

# The bit left on the end of the paper, after the notes, to give a pleasing look
LEAD_OUT_MM = 10

# The default diameter of a note
NOTE_DIAMETER_MM_DEFAULT = 2

# The colour of all of the guidance lines
COLOUR_GUIDANCE = "darkblue"

# The colour of the notes
COLOUR_NOTES = "black"

# The colour of the paper
COLOUR_BACKGROUND = "lightgray"

# The stroke-width of all of the lines
LINE_STROKE_WIDTH = 0.1

def lead_in_pixels(mm_per_pixel):
    '''Return the lead-in size in pixels'''
    return LEAD_IN_MM * mm_per_pixel

def lead_out_pixels(mm_per_pixel):
    '''Return the lead-out size in pixels'''
    return LEAD_OUT_MM * mm_per_pixel

def quaver_distance_pixels(speed_scale, mm_per_pixel):
    '''Return the quaver distance in pixels'''
    return QUAVER_DISTANCE_MM * mm_per_pixel / speed_scale

def note_step_pixels(mm_per_pixel):
    '''Return the vertical step between notes in pixels'''
    return NOTE_STEP_MM * mm_per_pixel

def margin_top_bottom_pixels(mm_per_pixel):
    '''Return the top or the bottom margin in pixels'''
    return MARGIN_TOP_BOTTOM_MM * mm_per_pixel

def paper_height_pixels(mm_per_pixel):
    '''Return the height of the paper in pixels'''
    return ((MARGIN_TOP_BOTTOM_MM * 2) + (NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1))) * mm_per_pixel

def paper_width_pixels(quaver_count, speed_scale, mm_per_pixel):
    '''Return the width of the paper in pixels'''
    return lead_in_pixels(mm_per_pixel) + (quaver_distance_pixels(speed_scale, mm_per_pixel) * quaver_count) + lead_out_pixels(mm_per_pixel)

def notes_total_height_pixels(mm_per_pixel):
    ''' Return the height of just the notes in the middle of the paper'''
    return NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1) * mm_per_pixel

def stroke_width_pixels(mm_per_pixel):
    ''' Return the stroke width of all of the lines'''
    return LINE_STROKE_WIDTH * mm_per_pixel

def note_radius_pixels(note_diameter_mm, mm_per_pixel):
    '''Return the radius of a note'''
    return note_diameter_mm * mm_per_pixel / 2

def svg_comment_top(speed_scale, mm_per_pixel):
    '''Return the string that is the comment at the top of the SVG file'''
    note_step = note_step_pixels(mm_per_pixel)
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    margin_top_bottom = margin_top_bottom_pixels(mm_per_pixel)
    lead_in = lead_in_pixels(mm_per_pixel)
    notes_total_height = notes_total_height_pixels(mm_per_pixel)
    comment = f'''<!--
This file was written by a Python script, plinky_plonky_strip_maker.py.  It represents notes on a paper strip, suitable for feeding into a plinky-plonky device such as this:

https://www.ebay.co.uk/itm/397246486151

...visible in action here:

https://youtu.be/XHdWuA18UNk

For more information on the project this was part of (and to find the Python script) see here:

https://www.meades.org/misc/musical_box/musical_box.html

The original paper strip had a distance between quavers beats of {QUAVER_DISTANCE_MM} mm in the X axis and a step of {NOTE_STEP_MM} mm between each note in the Y axis (leading to a total height of {NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)} mm for all {len(NOTE_STRING_LIST)} notes ({len(NOTE_STRING_LIST) - 1} steps)). SVG uses only pixels, not millimetres: in this file we use {mm_per_pixel} millimetre(s) per pixel and a speed scale factor of {speed_scale:g}, so the step between notes on the Y axis is {note_step} pixel(s) and the distance between quavers beats on the X axis is {quaver_distance:g} pixel(s).  Note that it is a fundamental limitation of these plinky-plonky machines that you cannot play the same note more than once within a crotchet beat.  This is because the little cams that get caught on the note holes and trigger the sound need to rotate a crotchet's distance around to spring the ping.

The top and bottom margins between the edge of the paper strip and the notes on the original paper strip was {MARGIN_TOP_BOTTOM_MM} mm, therefore it is {margin_top_bottom:g} pixel(s) here. In an SVG file the origin, 0/0, is at top left.  In the original paper strip there was a {LEAD_IN_MM} mm run-in between the point of the black triangle at the front of the strip and the first note, therefore the X coordinate of the first note here is {lead_in} pixel(s).  The Y coordinate of the lowest note on the original paper strip was at {MARGIN_TOP_BOTTOM_MM} mm (for the top margin) plus {NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)} mm (for the {len(NOTE_STRING_LIST)} notes), which makes it ({NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)} + {MARGIN_TOP_BOTTOM_MM}) * {mm_per_pixel} = {notes_total_height + margin_top_bottom:g} pixel(s).
-->'''
    return comment

def svg_open(quaver_count, speed_scale, mm_per_pixel):
    '''Return the opening tag of the SVG file, with comment'''
    lead_in = lead_in_pixels(mm_per_pixel)
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    lead_out = lead_out_pixels(mm_per_pixel)
    paper_height = paper_height_pixels(mm_per_pixel)
    paper_width = paper_width_pixels(quaver_count, speed_scale, mm_per_pixel)
    comment = f'''<!--
This is the outline of the paper strip, so it must be {lead_in} (for the lead-in) plus {quaver_distance:g} * the number of quavers beats wide (applying the speed scale factor of {speed_scale:g}), plus {lead_out} to give a pleasing margin, ({MARGIN_TOP_BOTTOM_MM} * 2 + {NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)}) * {mm_per_pixel} = {paper_height:g} high, with origin at 0,0.
-->'''
    return f'{comment}\n<svg width="{paper_width:g}" height="{paper_height:g}" viewBox="0 0 {paper_width:g} {paper_height:g}" fill="none" xmlns="http://www.w3.org/2000/svg">'

def svg_close():
    '''Return the closing tag of the SVG file'''
    return "</svg>\n"

def svg_rectangle(quaver_count, speed_scale, mm_per_pixel):
    '''Return the outline rectangle the SVG file, with comment above it'''
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    lead_in = lead_in_pixels(mm_per_pixel)
    lead_out = lead_out_pixels(mm_per_pixel)
    paper_width = paper_width_pixels(quaver_count, speed_scale, mm_per_pixel)
    paper_height = paper_height_pixels(mm_per_pixel)
    comment = f'''<!--
This rectangle is the outline of the paper strip, so it must be {lead_in} (for the lead-in) plus {quaver_distance:g} * the number of quaver beats wide (applying the speed scale factor of {speed_scale:g}), plus the {lead_out} pleasing end-margin, ({MARGIN_TOP_BOTTOM_MM} * 2 + {NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)}) * {mm_per_pixel} = {paper_height:g} high, with origin at 0,0.
-->'''
    return f'{comment}\n<rect id="outline" x="0" y="0" width="{paper_width:g}" height="{paper_height:g}" fill="{COLOUR_BACKGROUND}"/>'

def svg_horizontal_lines(quaver_count, speed_scale, mm_per_pixel):
    '''Return the horizontal visual-aid lines, with comment above'''
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    note_step = note_step_pixels(mm_per_pixel)
    x1 = lead_in_pixels(mm_per_pixel)
    x2 = x1 + quaver_distance_pixels(speed_scale, mm_per_pixel) * (quaver_count - 1)
    y = margin_top_bottom_pixels(mm_per_pixel)
    comment = f'''<!--
As a visual aid, add horizontal lines for each note.  In the X axis these all start at {x1} (i.e. after the lead-in) and are {quaver_distance:g} * the number of quaver beats long (with a speed scale factor of {speed_scale:g}).  In the Y axis the top-most note will be at {y:g}, subsequent notes at intervals of {note_step}. The note lines are given IDs for each note, of the form c0 for lowest C, c1 for the next C, c2 for the top-most C (which is the top-most note), etc.
-->'''
    lines_string = ""
    stroke_width = stroke_width_pixels(mm_per_pixel)
    for note in reversed(NOTE_STRING_LIST):
        lines_string += f'<line id="pitch {note}" x1="{x1}" y1="{y:g}" x2="{x2}" y2="{y:g}" stroke="{COLOUR_GUIDANCE}" stroke-width="{stroke_width}"/>\n'
        y += note_step
    return comment  + "\n" + lines_string

def svg_vertical_lines(quaver_count, speed_scale, mm_per_pixel):
    '''Return the vertical visual-aid lines, with comment above'''
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    y1 = margin_top_bottom_pixels(mm_per_pixel)
    y2 = y1 + notes_total_height_pixels(mm_per_pixel)
    x = lead_in_pixels(mm_per_pixel)
    comment = f'''<!--
As a visual aid, add vertical lines at each quaver beat: even ones (crotchets) solid, the ones in-between dotted.  In the X axis, the first solid line is at {x} (i.e. after the lead-in), the dotted line at {x + quaver_distance:2}, repeat this sequence every {(quaver_distance * 2):2} (with a speed scale factor of {speed_scale:g}).  In the Y axis all beat lines start at {y1:g} (for the top margin) and end at ({MARGIN_TOP_BOTTOM_MM} + {NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)}) * {mm_per_pixel} = {y2:g}.
-->'''
    lines_string = ""
    stroke_width = stroke_width_pixels(mm_per_pixel)
    for line in range(quaver_count):
        lines_string += f'<line id="quaver beat {line}" x1="{x}" y1="{y1:g}" x2="{x}" y2="{y2:g}" stroke="{COLOUR_GUIDANCE}" stroke-width="{stroke_width}"'
        if line % 2 != 0 and line != quaver_count:
            #  Even lines and the last line are solid, the others are dashed
            lines_string += f' stroke-dasharray="{quaver_distance}"'
        lines_string += '/>\n'
        x += quaver_distance
    return comment + "\n" + lines_string

def svg_arrow(mm_per_pixel):
    '''Return the arrow at the front of the paper strip, with comment above'''
    lead_in = lead_in_pixels(mm_per_pixel)
    note_radius = note_radius_pixels(NOTE_DIAMETER_MM_DEFAULT, mm_per_pixel)
    paper_height = paper_height_pixels(mm_per_pixel)
    comment = f'''<!--
The original paper strip had a triangular pointer on the front, which is maintained here.  It is within the lead-in and is the same size as the notes on the original paper strip.
-->'''
    return f'{comment}\n<polygon id="arrow" points="{lead_in  / 2:g},{paper_height / 2:g} {lead_in / 2 + (note_radius * 2):g},{(paper_height / 2) - note_radius:g}, {(lead_in / 2) + (note_radius * 2):g},{(paper_height / 2) + note_radius:g}" fill="{COLOUR_GUIDANCE}"/>'

def svg_marker(mm_per_pixel):
    '''Return the marker at the front/bottom of the paper strip, with comment above'''
    lead_in = lead_in_pixels(mm_per_pixel)
    note_radius = note_radius_pixels(NOTE_DIAMETER_MM_DEFAULT, mm_per_pixel)
    paper_height = paper_height_pixels(mm_per_pixel)
    comment = f'''<!--
It is useful to have a marker in the bottom left of the paper strip, after the lead-in, at the bottom of the Y axis and where the first note could occur in the X axis.  Make it a triangle of the same dimension as the diameter of the notes on the original paper strip, but narrower to avoid confusion with the forward-facing arrow.
-->'''
    return f'{comment}\n<polygon id="marker_bottom_left" points="{lead_in - (note_radius / 2):g},{paper_height - note_radius:g} {lead_in + (note_radius / 2):g},{paper_height - note_radius:g}, {lead_in:g},{paper_height:g}" fill="{COLOUR_GUIDANCE}"/>'

def svg_notes(note_list, note_diameter_mm, speed_scale, mm_per_pixel):
    '''Return the notes, with comment above'''
    note_radius = note_radius_pixels(note_diameter_mm, mm_per_pixel)
    quaver_distance = quaver_distance_pixels(speed_scale, mm_per_pixel)
    note_step = note_step_pixels(mm_per_pixel)
    comment = f'''<!--
And finally, the important part: the notes.  In the original paper strip notes were {NOTE_DIAMETER_MM_DEFAULT} mm in diameter; here they are {note_diameter_mm} mm in diameter and therefore are {note_radius:g} pixel(s) in radius.
-->'''
    lines_string = ""
    cx = lead_in_pixels(mm_per_pixel)
    c0 = margin_top_bottom_pixels(mm_per_pixel) + notes_total_height_pixels(mm_per_pixel)
    sneaky_semiquaver = False
    for count, note in enumerate(note_list):
        if note >= 0 and note < len(NOTE_STRING_LIST):
            cy = c0 - note_step * note
            lines_string += f'<circle id="note {count} {NOTE_STRING_LIST[note]}" cx="{cx:g}" cy="{cy:g}" r="{note_radius:g}" fill="{COLOUR_NOTES}"/>\n'
        else:
            if note >= len(NOTE_STRING_LIST):
                if sneaky_semiquaver:
                    # If we were on a sneaky semiquaver, line us up again
                    cx += quaver_distance / 2
                    sneaky_semiquaver = False
                else:
                    # New beat
                    cx += quaver_distance
            else:
                # Go back for a sneaky semiquaver
                sneaky_semiquaver = True
                cx -= quaver_distance / 2
                    
    return comment + "\n" + lines_string

def write_svg(output_file_handle, note_list, quaver_count, mm_per_pixel, speed_scale, note_diameter_mm):
    '''Write all the bits of an SVG file, based on the note list'''
    output_file_handle.write(svg_comment_top(speed_scale, mm_per_pixel) + "\n\n")
    output_file_handle.write(svg_open(quaver_count, speed_scale, mm_per_pixel) + "\n\n")
    output_file_handle.write(svg_rectangle(quaver_count, speed_scale, mm_per_pixel) + "\n\n")
    output_file_handle.write(svg_horizontal_lines(quaver_count, speed_scale, mm_per_pixel) + "\n")
    output_file_handle.write(svg_vertical_lines(quaver_count, speed_scale, mm_per_pixel) + "\n")
    output_file_handle.write(svg_arrow(mm_per_pixel) + "\n\n")
    output_file_handle.write(svg_marker(mm_per_pixel) + "\n\n")
    output_file_handle.write(svg_notes(note_list, note_diameter_mm, speed_scale, mm_per_pixel) + "\n")
    output_file_handle.write(svg_close())

def parse(input_data, speed_scale):
    '''Parse an input data sring and return a list of notes and gaps, where
       a note is an index into NOTE_STRING_LIST and the move to a new
       quaver beat is indicated by an out of range positive value, a quarter
       beat offset (semiquaver) is indicated by an out of range negative value'''
    # input_data is a string that can contain a note from NOTE_STRING_LIST,
    # a new-line "\n" or whitespace
    note_letter = None
    note_count = 0
    semiquaver_beat_count = 0
    # output data a list of integers, being an index into the NOTE_STRING_LIST
    # for a note or anything else for a quaver beat gap
    output_list = []
    # Note history is intended to remember all notes that were played
    # within the last crotchet beat, since the plinky-plonky's mechanism
    # cannot re-play notes faster than that.  It should contain pairs
    # of values: a note index and the semiquaver_beat_count it occurred on
    note_history_list = []
    for input_count, character in enumerate(input_data):
        got_match = False
        if note_letter:
            # Have a note first character, this should be the second character
            # of the note
            for note_index, note in enumerate(NOTE_STRING_LIST):
                if note[0] == note_letter and note[1] == character:
                    output_list.append(note_index)
                    note_history_list.append([note_index, semiquaver_beat_count])
                    note_count += 1
                    got_match = True
                    # Reset the note letter for the next one
                    note_letter = None
                    break
            if not got_match:
                print(f"Warning: found note fragment '{note_letter}'" \
                      f" at offset {input_count - 1} (counting from 0)" \
                       " in input file.")
                # Reset the note letter if we don't get a match on the second character
                note_letter = None
        if not got_match:
            if character == '\n':
                # It is a new-line: append a positive out of range value
                output_list.append(len(NOTE_STRING_LIST))
                semiquaver_beat_count += 2
                got_match = True
            else:
                if character == '^':
                    # It is a sneaky semiquaver line: append a negative value
                    output_list.append(-1)
                    if semiquaver_beat_count >= 0:
                        semiquaver_beat_count -= 1
                    got_match = True
                else:
                    # Could be the first letter of a note, find it in the list
                    for note in NOTE_STRING_LIST:
                        if note[0] == character.lower():
                            note_letter = note[0]
                            got_match = True
                            break
        if not got_match and not character.isspace():
            # If we've got no match and the character is not whitespace
            # print a warning
            print(f"Warning: found unexpected character '{character}'" \
                  f" at offset {input_count - 1} (counting from 0) in" \
                  f" input file.")
    # Go through the note history list and warn if a note
    # has been repeated within a crotchet beat
    warned = False
    last_crotchet_beat_list = []
    last_semiquaver_beat = -1
    for note_history in note_history_list:
        if note_history[1] != last_semiquaver_beat:
            last_semiquaver_beat = note_history[1]
            # We've moved on at least one semiquaver beat,
            # create a new last crotchet beat list
            # containing only the notes within the last
            # crotchet beat
            scaled_crotchet = speed_scale * 4
            new_list = []
            for last_crotchet_beat in last_crotchet_beat_list:
                if last_crotchet_beat[1] > last_semiquaver_beat - scaled_crotchet:
                    new_list.append(last_crotchet_beat)
            last_crotchet_beat_list = new_list
        # Check if this note has been played within the
        # last crotchet beat
        for last_crotchet_beat in last_crotchet_beat_list:
           if last_crotchet_beat[0] == note_history[0]:
               print(f"Warning: around quaver beat {int(last_semiquaver_beat / 2):g}"
                     f" (counting from zero) note {NOTE_STRING_LIST[note_history[0]]}"
                     " was played twice within the same crotchet beat!")
               warned = True
        last_crotchet_beat_list.append(note_history)
    if warned:
        print("NOTE: you may be able to remove the crotchet beat warnings above" \
              " by decreasing the speed scale (and hence increasing the space between"
              " notes) with the -s option, see command-line help (-h) for details.")
    return output_list

def process(input_file, output_file, mm_per_pixel, speed_scale, note_diameter_mm):
    '''Process the input file, write the output, return 0 on success'''
    # Assume failure
    result_code = 1
    print(f"Processing file {input_file}, writing output to {output_file}.")
    try:
        with open(output_file, "w") as output_handle:
            try:
                with open(input_file, "r") as input_handle:
                    # Have both files, let's process!
                    input_string = input_handle.read()
                    note_list = parse(input_string, speed_scale)
                    note_count = 0
                    quaver_count = 0
                    semiquaver_count = 0
                    for item in note_list:
                        if item >= 0 and item < len(NOTE_STRING_LIST):
                            note_count += 1
                        else:
                            if item >= len(NOTE_STRING_LIST):
                                quaver_count += 1
                            else:
                                semiquaver_count += 1
                    print(f"Found {note_count} note(s) and {quaver_count} quaver beat(s)" \
                          f"and {semiquaver_count} sneaky semiquaver(s).")
                    length_notes = quaver_distance_pixels(speed_scale, mm_per_pixel) * quaver_count / mm_per_pixel
                    length_lead_int_out = (lead_in_pixels(mm_per_pixel) + lead_out_pixels(mm_per_pixel)) / mm_per_pixel
                    print(f"When printed the strip of paper would be {(length_lead_int_out + length_notes):g} mm long," \
                          f" of which {length_notes:g} mm is the notes.")
                    # Write out the SVG file
                    print(f"Writing to {output_file}")
                    # +1 below as there might not be a quaver advance (i.e. a newline) on
                    # the end, best be safe
                    write_svg(output_handle, note_list, quaver_count + 1, mm_per_pixel, speed_scale, note_diameter_mm)
                    print(f"{output_file} successfully written.")
                    result_code = 0
            except IOError as e:
                print(f"Unable to open input file {input_file}: \"{e}\".")
    except IOError as e:
        print(f"Unable to open output file {output_file}: \"{e}\".")

    return result_code

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=("A script to make an SVG file that represents"
                                                  " music to a plinky-plonky machine.\n\n"
                                                  "Give it a text file containing notes (see below"
                                                  " for the format) and these"
                                                  " will be converted into dots on an SVG image"
                                                  " scaled to the same relative proportions as"
                                                  " the paper strips used in those machines, "
                                                  " i.e. " + "{:g}".format(MARGIN_TOP_BOTTOM_MM * 2 +
                                                  (NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1))) +
                                                  " mm wide with " + str(len(NOTE_STRING_LIST)) +
                                                  " notes across a " +
                                                  str(NOTE_STEP_MM * (len(NOTE_STRING_LIST) - 1)) +
                                                  " mm span in the middle of the paper and"
                                                  " therefore margins of " + str(MARGIN_TOP_BOTTOM_MM) +
                                                  " mm either side and a note diameter of " +
                                                  str(NOTE_DIAMETER_MM_DEFAULT) + " mm unless otherwise"
                                                  " specified. SVG does not use absolute units only"
                                                  " pixels: the scale used here is " +
                                                  str(MILLIMETRES_PER_PIXEL_DEFAULT) +
                                                  " pixels to the millimetre unless otherwise specified.\n\n"
                                                  "The input text file should contain notes of the form"
                                                  " c0 (case-insensitive) for lowest C, d0 for D above that, etc."
                                                  " to g0, then c1 for next highest C, all the"
                                                  " way up to C2 for top C.  Notes on the same line"
                                                  " are on one quaver beat, notes on the next line"
                                                  " are on the next quaver beat; a quaver beat with no"
                                                  " notes is indicated by a blank line, so for instance:\n\n"
                                                  "  c0f1\n  a0\n\n  b0c2\n\n"
                                                  "...represents bottom C and F in the next octave up"
                                                  " played at the same time, followed a quaver later"
                                                  " by A in the first octave, followed by a quaver gap,"
                                                  " followed by top C.\n\n"
                                                  "IMPORTANT: it is a fundamental limitation of these"
                                                  " plinky-plonky machines that you cannot play the"
                                                  " same note more than once within a crotchet beat."
                                                  " This is because the little cams that get caught on"
                                                  " the note holes and trigger the sound need to rotate"
                                                  " a crotchet's distance around to spring the ping."
                                                  " This script will emit a warning if it spots such"
                                                  " a problem in the input data.  You may be able to"
                                                  " work around these issues by increasing the length"
                                                  " between notes.  Do this by setting a speed scale"
                                                  " factor smaller than one (e.g. 0.5 if you repeat"
                                                  " the same note within a crotchet spacing, 0.25 if you"
                                                  " repeat the same note within a sneaky semiquaver"
                                                  " spacing (see below)).  Of course, you will then"
                                                  " have to increase the speed your plinky-plonky machine"
                                                  " is cranked at to get everything back to normal.\n\n"
                                                  "One more thing: you _might_ want to sneak in a note"
                                                  " between two others, at a semiquaver.  There is no"
                                                  " guarantee this will work (you might want to use the"
                                                  " -s option to increase the horizontal spacing or the"
                                                  " -d option to reduce the hole size if they get close"
                                                  " to each other) but if you want to try it, put a"
                                                  " ^ (hat) character at the START of that line and"
                                                  " the notes will be snuck in between the quaver beats"
                                                  " of the adjacent lines.\n\n"
                                                  "Notes may be separated by whitespace for readability."),
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('input_file', help=("a file containing notes and gaps"))
    parser.add_argument('output_file', nargs='?', help=("output SVG file name: if not provided the"
                                                        " input file name will be used with an .svg"
                                                        " extension. ANY EXISTING FILE WILL BE"
                                                        " OVERWRITTEN"))
    parser.add_argument('-s', type=float, default=SPEED_SCALE_DEFAULT, help=("use this to slow down (e.g. 0.5 would"
                                                                           " double the distance between notes) or"
                                                                           " speed up (2 would halve the distance"
                                                                           " between notes), " + 
                                                                           str(SPEED_SCALE_DEFAULT) +
                                                                           " if not specified"))
    parser.add_argument('-m', type=int, default=MILLIMETRES_PER_PIXEL_DEFAULT, help=("the number of millimetres"
                                                                                     " per pixel, " + 
                                                                                     str(MILLIMETRES_PER_PIXEL_DEFAULT) +
                                                                                     " if not specified"))
    parser.add_argument('-d', type=float, default=NOTE_DIAMETER_MM_DEFAULT, help=("the diameter of a note in millimetres, " +
                                                                                   str(NOTE_DIAMETER_MM_DEFAULT) + " if not specified"))

    args = parser.parse_args()

    # Result code, 0 for success
    result_code = 1

    if not os.path.isfile(args.input_file):
        print(f"Unable to find input file \"{args.input_file}\"!")
    else:
        output_file = args.output_file
        if not output_file:
            output_file = os.path.basename(args.input_file).split('.')[0] + ".svg"
        result_code = process(args.input_file, output_file, args.m, args.s, args.d)

    sys.exit(result_code)
