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

'''Drive a unipolar (5 wire) stepper motor using a ULN2003 driver from a Raspberry Pi.'''

import pigpio
from time import sleep

# The colours of the wires coming out of the back of a
# 5-wire 24BYJ48 unipolar stepper motor are quite variable,
# hence the arrangement is described in detail here.
#
# A unipolar stepper motor's coils are usually labelled as
# follows:
#
#                  A1 o---8     -
#                         8  -     -
#                       --8 -       -
#                       | 8  -     -
#                  A2 o---8     -
#                       |   888888888
#                       ----|---|   |
#                      /    |       |
#                     o     o       o
#                   C       B1      B2
#
# The wires coming out of the back of the stepper motor, when
# looking at the motor so that you can see the spindle, are
# as follows:
#                              _
#                           -     -
#                         -    O    -
#                         -         -
#                           -     -
#                           -     -
#                        |  |  |  |  |
#                        B1 A2 C  A1 B2
#
# On the particular motor I happen to be using, the wire colours
# are:
#
#  B1: blue
#  A2: brown
#  C:  red
#  A1: black
#  B2: yellow

# The motor connections and which GPIO they are connected to,
# ordered 0 = A1, 1 = A2, 2 = B1, 3 = B2, ignoring connection
# C, the common connection, which is connected to the supply voltage
gpio_list = [24, 23, 22, 25]

# The simplest GPIO sequence, where only a single
# terminal is active at any one time,  is:
#
#  Step      Coil A1  Coil A2  Coil B1  Coil B2
#   0          1        0        0        0
#   1          0        0        1        0
#   2          0        1        0        0
#   3          0        0        0        1
#
simple_step_list = [(0, -1), (2, -1), (1, -1), (3, -1)] 

# The GPIO sequence for maximum torque and full steps is
# four steps and pairs of terminals energised:
#
#  Step      Coil A1  Coil A2  Coil B1  Coil B2
#   0          1        0        1        0
#   1          0        1        1        0
#   2          0        1        0        1
#   3          1        0        0        1
#
max_torque_step_list = [(0, 2), (1, 2), (1, 3), (0, 3)] 

# The half-step (i.e. more accuracy but less torque)
# GPIO sequence is:
#
#  Step      Coil A1  Coil A2  Coil B1  Coil B2
#   0          1        0        0        0
#   1          1        0        1        0
#   2          0        0        1        0
#   3          0        1        1        0
#   4          0        1        0        0
#   5          0        1        0        1
#   6          0        0        0        1
#   7          1        0        0        1
#
half_step_list = [(0, -1), (0, 2), (2, -1), (1, 2), (1, -1), (1, 3), (3, -1), (0, 3)] 

# The 2BYJ48 has close to 64 steps per revolution
steps_per_revolution = 64

# The 2BYJ48-034 has a gearing reduction of close to 64:1
gearing_reduction = 63.68395

# This just for debug prints
coil_list = ["A1", "A2", "B1", "B2"]
wire_list = ["black", "brown", "blue", "yellow"]

def step_string(step):
    '''Return a printable string of the coils for a given step'''
    str = ""
    if step[0] >= 0:
        str = f"{coil_list[step[0]]}"
    if step[1] >= 0 and step[1] != step[0]:
        str += f" and {coil_list[step[1]]}"
    return str

def non_wave_step(pi, step_list, count, step_length_ms):
    '''Using GPIO, energise the GPIOs in step list, but without using a GPIOD wave'''
    '''and printing lots of debug to illustrate the workings'''
    previous_step = (-1, -1)
    for x in range (count):
       for index, step in enumerate(step_list):
           print(f"Step {index}")
           if previous_step[0] >= 0 or previous_step[1] >= 0:
               print(f"  Previously energized {step_string(previous_step)},", end="")
               # Switch off a previously-on GPIO if it is not still meant to be on
               for gpio_index in previous_step:
                   if gpio_index >= 0:
                       if gpio_index != step[0] and gpio_index != step[1]:
                           print(f" switching off {coil_list[gpio_index]}", end="")
                           pi.write(gpio_list[gpio_index], 0)
                       else:
                           print(f" NOT switching off {coil_list[gpio_index]} ('cos we need it on this step)", end="")
               print("")     
           # Remember the current pair
           previous_step = step
           # Switch on the current pair
           print(f"  Energise {step_string(step)}")
           for gpio_index in step:
               if gpio_index >= 0:
                   pi.write(gpio_list[gpio_index], 1)
           sleep(step_length_ms / 1000)

def wave_step(pi, step_list, count, step_length_ms):
    '''The GPIOD wave version: no debug prints during operation, accurately timed'''
    pulse_list = []
    print("Assembling wave.")
    # Set previous_step to the last entry in step_list so that it is circular 
    previous_step = step_list[len(step_list) - 1]
    for index, step in enumerate(step_list):
        print(f"Step {index}")
        # Create the "off" bitmap
        off_gpio_bitmap = 0
        if previous_step[0] >= 0 or previous_step[1] >= 0:
            print(f"  Previously on {step_string(previous_step)},", end="")
            # Switch off a previously-on GPIO if it is not still meant to be on
            for gpio_index in previous_step:
                if gpio_index >= 0:
                    if gpio_index != step[0] and gpio_index != step[1]:
                        print(f" adding {coil_list[gpio_index]} to off bitmap", end="")
                        off_gpio_bitmap |= 1 << gpio_list[gpio_index]
                    else:
                        print(f" NOT adding {coil_list[gpio_index]} to off bitmap ('cos we need it on this step)", end="")
            print("")
        print(f"  Off bit-map is 0x{off_gpio_bitmap:08x}")
        # Remember the current pair
        previous_step = step
        # Create the "on" bit-map
        on_gpio_bitmap = 0
        print(f"  Switch on {step_string(step)}")
        for gpio_index in step:
            if gpio_index >= 0:
                on_gpio_bitmap |= 1 << gpio_list[gpio_index]
        print(f"  On bit-map is 0x{on_gpio_bitmap:08x}")
        # Add the pulse
        print(f"  Pulse will be \"pigpio.pulse(0x{on_gpio_bitmap:08x}, 0x{off_gpio_bitmap:08x}, {step_length_ms * 1000})\"")
        pulse_list.append(pigpio.pulse(on_gpio_bitmap, off_gpio_bitmap, step_length_ms * 1000))

    # Run the wave
    pi.wave_clear()
    pi.wave_add_generic(pulse_list)
    pulses = pi.wave_create()
    duration_seconds = step_length_ms * len(step_list) * count / 1000
    print(f"Waving {count:g} time(s), which should take {duration_seconds:g} second(s).")
    pi.wave_send_repeat(pulses)
    sleep(duration_seconds)
    pi.wave_tx_stop()
    pi.wave_delete(pulses)

# Connect to pipgiod
pi = pigpio.pi()

if pi.connected:

    # Set up the GPIOs
    for gpio in gpio_list:
        pi.set_mode(gpio, pigpio.OUTPUT)
        pi.write(gpio, 0)

    try:
        # Run the motor with the given step list, number of step cycles
        # and step duration in milliseconds
        geared_step_cycles_per_revolution = steps_per_revolution * gearing_reduction / len(max_torque_step_list)
        wave_step(pi, max_torque_step_list, geared_step_cycles_per_revolution, 3)
    except KeyboardInterrupt:
        print("Stopped by user")
    finally:
        # Tidy up
        pi.wave_tx_stop()
        for gpio in gpio_list:
            pi.write(gpio, 0)
        pi.stop()
        print("Finished")
else:
    print("pigpiod is not running: sudo pigpiod please!")

