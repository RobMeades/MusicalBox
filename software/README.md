# Introduction
Here find all of the software for the musical box:

- `esp32` contains all of the C code for the "modules" consisting of an ESP32 device, a TMC2209 stepper motor driver and one or more QRD1114 sensors,
- `pi` contains the stuff for the Raspberry Pi, the controller of the modules, Python scripts and explanatory `README.md`s for set up,
- `protocol` defines the communications protocol between the Pi and the modules, defined as a C header file and a Python module generated from that.

# Description
During development the ESP32 C code is compiled differently for different types of modules: there's a stand that rotates, a lift that rises and falls, a plinky-plonky that plays music and six doors that open and close; define the pin names for the sensor types (or if a device has no sensors, as in the case of the stand, then just an on/off #define) and you get code for that thing.  For instance, define `CONFIG_STEPPER_LIFT_LIMIT_PIN` and `CONFIG_STEPPER_LIFT_DOWN_PIN` and you get an executable that operates the lift.  The `https_server.py` python script running on the Raspberry Pi, which can serve OTA updates to the devices, can be run in "differentiated" mode (`-d` parameter) and it will then serve the right executable to the right module based on their [fixed] IP addresses.

Once development of all the individual bits has been done and the whole musical box has been assembled, it won't work without coordination (e.g. all of the doors have to open at the same time).  At this point, `CONFIG_STEPPER_PRODUCTION_MODE` must be defined.  Now the same binary image is used on all of the modules, (`https_server.py` no longer runs in differentiated mode).  Knowledge of things like the velocity at which to run the motor, the current supplied to the motor and how long to run the motor for a given task, is devolved to the Raspberry Pi.