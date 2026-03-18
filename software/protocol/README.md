# Introduction
This directory defines the commmunications protocol between the ESP32 "modules" and the controlling Raspberry Pi:

- `protocol.h`: the master definition,
- `generate_python_protocol_module.py`: parses `protocol.h` and writes `protocol.py`,
- `protocol.py`: the output of `generate_python_protocol_module.py`, a Python module that can be used in the script running on the Raspberry Pi that controls everyting.

The protocol is intended to be run over a lossless, ordered, bearer (e.g. a TCP socket).