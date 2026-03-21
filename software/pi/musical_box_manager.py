##!/usr/bin/env python3

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

import socket
import select
import queue
import threading
import time
import re
import argparse
from time import sleep
from datetime import datetime
from pathlib import Path
from typing import Optional, Union, Dict, List, Tuple
from esp32_server import Esp32Server

# Add the protocol directory to Python path
# Get the directory where THIS script is located
import sys
from pathlib import Path
script_dir = Path(__file__).resolve().parent

# Navigate up to the common directory where protocol.h and protocol.py live
protocol_dir = script_dir.parent / 'protocol'
sys.path.insert(0, str(protocol_dir))

import protocol

# The default listening port
ESP32_PORT_DEFAULT = 5000

class MusicalBoxManager(Esp32Server):
    """Specialised manager for the Musical Box stuff"""
    def __init__(self):
        # Call parent constructor
        super().__init__()
        self.log_callback = None

        # Track state for each device type
        self.lift_state = {}  # ip -> current state (from STATE_LIFT_*)
        self.door_states = {}  # ip -> current state (from STATE_DOOR_*)
        self.stand_state = {}  # ip -> current state (from STATE_STAND_*)
        self.plinky_plonky_state = {}  # ip -> current state (from STATE_PLINKY_PLONKY_*)
        
        # Track sensor states
        self.lift_sensors = {}  # ip -> {'down': bool, 'limit': bool}
        self.door_sensors = {}  # ip -> {'open': bool}
        self.plinky_plonky_sensors = {}  # ip -> {'reference': bool}
        
        # Track pending queries (for correlating responses)
        self.pending_queries = {}  # (ip, reference) -> query_type
        
        # Track current reference for each device (already in self.devices)
        # We'll use self.devices[ip]["reference"] for commands/queries
        
        # Formatter for pretty prints (optional)
        self.formatter = Formatter()

    def set_log_callback(self, callback):
        """Set a callback function for logging"""
        self.log_callback = callback
    
    def _log(self, message):
        """Log a message using callback if available, otherwise print"""
        if self.log_callback:
            self.log_callback(message)
        else:
            print(message)

    def _process_message(self, ip, msg):
        """Process incoming messages and update internal state"""
        device_name = self.devices[ip]["name"]
        
        if isinstance(msg, protocol.RspMsg):
            return self._process_response(ip, device_name, msg)
        elif isinstance(msg, protocol.IndMsg):
            return self._process_indication(ip, device_name, msg)
    
    def _process_response(self, ip, device_name, msg):
        """Process a response message"""
        cmd_or_qry = msg.cmd_or_qry
        ref = msg.reference
        status = msg.status
        value = msg.value
        
        print(f"{device_name}: response to {self.formatter.cmd_or_qry(cmd_or_qry)}, "
              f"ref={ref}: status={self.formatter.status(status)} (value={value})")
        
        # Check if this is a response to a query we sent
        query_key = (ip, ref)
        if query_key in self.pending_queries:
            query_type = self.pending_queries.pop(query_key)
            self._handle_query_response(ip, device_name, query_type, value, status)
        
        return True
    
    def _handle_query_response(self, ip, device_name, query_type, value, status):
        """Handle response to a specific query"""
        if status != protocol.Status.STATUS_OK:
            print(f"  Query {self.formatter.qry(query_type)} failed with status {self.formatter.status(status)}")
            return
        
        # Update state based on query type
        if query_type == protocol.Qry.QRY_SYSTEM_STEPPER_STATE:
            # This could be for lift, stand, or plinky-plonky
            # We need to know which device it came from
            if device_name.startswith("lift"):
                self.lift_state[ip] = value
                print(f"  Lift state: {self.formatter.state(value)}")
            elif device_name.startswith("stand"):
                self.stand_state[ip] = value
                print(f"  Stand state: {self.formatter.state(value)}")
            elif device_name.startswith("plinky"):
                self.plinky_plonky_state[ip] = value
                print(f"  Plinky state: {self.formatter.state(value)}")
        
        elif query_type == protocol.Qry.QRY_LIFT_SENSOR_DOWN:
            self.lift_sensors.setdefault(ip, {})['down'] = bool(value)
            print(f"  Lift down sensor: {'TRIGGERED' if value else 'clear'}")
        
        elif query_type == protocol.Qry.QRY_LIFT_SENSOR_LIMIT:
            self.lift_sensors.setdefault(ip, {})['limit'] = bool(value)
            print(f"  Lift limit sensor: {'TRIGGERED' if value else 'clear'}")
        
        elif query_type == protocol.Qry.QRY_PLINKY_PLONKY_SENSOR_REFERENCE:
            self.plinky_plonky_sensors.setdefault(ip, {})['reference'] = bool(value)
            print(f"  Plinky reference sensor: {'TRIGGERED' if value else 'clear'}")
        
        elif query_type == protocol.Qry.QRY_DOOR_SENSOR_OPEN:
            self.door_sensors.setdefault(ip, {})['open'] = bool(value)
            print(f"  Door open sensor: {'TRIGGERED' if value else 'clear'}")
    
    def _process_indication(self, ip, device_name, msg):
        """Process an indication message"""
        ind = msg.ind
        value = msg.value
        
        self._log(f"{device_name}: indication {self.formatter.ind(ind)} (value={value})")
        
        if ind == protocol.Ind.IND_SYSTEM_STEPPER_TARGET_END:
            self._log(f"  Stepper movement completed with status {self.formatter.status(value)}")
            # If this is a door, query its state
            if device_name.startswith("door"):
                self._log(f"  Door movement completed, querying door state...")
                self.query_door_state(ip)
        
        elif ind == protocol.Ind.IND_DOOR_SENSOR_TRIGGERED_DOOR_OPEN:
            self.door_sensors.setdefault(ip, {})['open'] = bool(value)

        elif ind == protocol.Ind.IND_LIFT_SENSOR_TRIGGERED_LIFT_DOWN:
            self.lift_sensors.setdefault(ip, {})['down'] = bool(value)
        
        elif ind == protocol.Ind.IND_LIFT_SENSOR_TRIGGERED_LIFT_LIMIT:
            self.lift_sensors.setdefault(ip, {})['limit'] = bool(value)
        
        elif ind == protocol.Ind.IND_PLINKY_PLONKY_SENSOR_TRIGGERED_REFERENCE:
            self.plinky_plonky_sensors.setdefault(ip, {})['reference'] = bool(value)
        
    
    # ===== Status APIs =====
    
    def get_lift_state(self, ip=None):
        """Get lift state for a specific lift or all lifts"""
        if ip:
            return self.lift_state.get(ip)
        return self.lift_state.copy()
    
    def is_lift_up(self, ip=None):
        """Check if lift is at the up position"""
        if ip:
            return self.lift_state.get(ip) == protocol.State.STATE_LIFT_STOPPED_UP
        return any(state == protocol.State.STATE_LIFT_STOPPED_UP for state in self.lift_state.values())
    
    def is_lift_down(self, ip=None):
        """Check if lift is at the down position"""
        if ip:
            return self.lift_state.get(ip) == protocol.State.STATE_LIFT_STOPPED_DOWN
        return any(state == protocol.State.STATE_LIFT_STOPPED_DOWN for state in self.lift_state.values())
    
    def is_lift_moving(self, ip=None):
        """Check if lift is currently moving"""
        if ip:
            state = self.lift_state.get(ip)
            return state in [protocol.State.STATE_LIFT_RISING, protocol.State.STATE_LIFT_LOWERING]
        return any(state in [protocol.State.STATE_LIFT_RISING, protocol.State.STATE_LIFT_LOWERING] 
                  for state in self.lift_state.values())
    
    def get_door_state(self, ip=None):
        """Get door state for a specific door or all doors"""
        if ip:
            return self.door_states.get(ip)
        return self.door_states.copy()
    
    def is_door_open(self, ip=None):
        """Check if door is open"""
        if ip:
            return self.door_states.get(ip) == protocol.State.STATE_DOOR_STOPPED_OPEN
        return any(state == protocol.State.STATE_DOOR_STOPPED_OPEN for state in self.door_states.values())
    
    def is_door_possibly_closed(self, ip=None):
        """Check if a door might be closed: we can't actually tell as there no sensor for closed"""
        if ip:
            return self.door_states.get(ip) == protocol.State.STATE_DOOR_STOPPED_CLOSED
        return any(state == protocol.State.STATE_DOOR_STOPPED_CLOSED for state in self.door_states.values())
    
    def is_door_moving(self, ip=None):
        """Check if door is currently moving"""
        if ip:
            state = self.door_states.get(ip)
            return state in [protocol.State.STATE_DOOR_OPENING, protocol.State.STATE_DOOR_CLOSING]
        return any(state in [protocol.State.STATE_DOOR_OPENING, protocol.State.STATE_DOOR_CLOSING] 
                  for state in self.door_states.values())
    
    def get_stand_state(self, ip=None):
        """Get stand state"""
        if ip:
            return self.stand_state.get(ip)
        return self.stand_state.copy()
    
    def is_stand_rotating(self, ip=None):
        """Check if stand is rotating"""
        if ip:
            state = self.stand_state.get(ip)
            return state in [protocol.State.STATE_STAND_ROTATING_CLOCKWISE, 
                           protocol.State.STATE_STAND_ROTATING_ANTICLOCKWISE]
        return any(state in [protocol.State.STATE_STAND_ROTATING_CLOCKWISE, 
                           protocol.State.STATE_STAND_ROTATING_ANTICLOCKWISE] 
                  for state in self.stand_state.values())
    
    def get_plinky_plonky_state(self, ip=None):
        """Get plinky-plonky state"""
        if ip:
            return self.plinky_plonky_state.get(ip)
        return self.plinky_plonky_state.copy()
    
    def is_plinky_plonky_at_reference(self, ip=None):
        """Check if plinky-plonky is at reference position"""
        if ip:
            return self.plinky_plonky_state.get(ip) == protocol.State.STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE
        return any(state == protocol.State.STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE 
                  for state in self.plinky_plonky_state.values())
    
    def is_plinky_plonky_playing(self, ip=None):
        """Check if plinky-plonky is playing"""
        if ip:
            return self.plinky_plonky_state.get(ip) == protocol.State.STATE_PLINKY_PLONKY_PLAYING
        return any(state == protocol.State.STATE_PLINKY_PLONKY_PLAYING 
                  for state in self.plinky_plonky_state.values())
    
    # ===== Enhanced Command Methods with Automatic State Tracking =====
    
    def send_query_with_tracking(self, ip, query, query_type):
        """Send a query and track it for response handling"""
        ref = self.devices[ip]["reference"]
        self.pending_queries[(ip, ref)] = query_type
        qry_msg = protocol.QryMsg(query, ref)
        success = self.send_query(ip, qry_msg)
        if success:
            self.devices[ip]["reference"] = self.next_reference(ref)
        else:
            # Remove tracking if send failed
            self.pending_queries.pop((ip, ref), None)
        return success
    
    def query_lift_state(self, ip):
        """Query the current lift state"""
        return self.send_query_with_tracking(ip, protocol.Qry.QRY_SYSTEM_STEPPER_STATE, protocol.Qry.QRY_SYSTEM_STEPPER_STATE)
    
    def query_lift_sensors(self, ip):
        """Query both lift sensors"""
        self.send_query_with_tracking(ip, protocol.Qry.QRY_LIFT_SENSOR_DOWN, protocol.Qry.QRY_LIFT_SENSOR_DOWN)
        self.send_query_with_tracking(ip, protocol.Qry.QRY_LIFT_SENSOR_LIMIT, protocol.Qry.QRY_LIFT_SENSOR_LIMIT)
    
    def query_door_state(self, ip):
        """Query door state"""
        return self.send_query_with_tracking(ip, protocol.Qry.QRY_SYSTEM_STEPPER_STATE, protocol.Qry.QRY_SYSTEM_STEPPER_STATE)
    
    def query_door_sensor(self, ip):
        """Query door open sensor"""
        return self.send_query_with_tracking(ip, protocol.Qry.QRY_DOOR_SENSOR_OPEN, protocol.Qry.QRY_DOOR_SENSOR_OPEN)
    
    def query_plinky_plonky_state(self, ip):
        """Query plinky-plonky state"""
        return self.send_query_with_tracking(ip, protocol.Qry.QRY_SYSTEM_STEPPER_STATE, protocol.Qry.QRY_SYSTEM_STEPPER_STATE)
    
    def query_plinky_plonky_sensor(self, ip):
        """Query plinky-plonky reference sensor"""
        return self.send_query_with_tracking(ip, protocol.Qry.QRY_PLINKY_PLONKY_SENSOR_REFERENCE, 
                                             protocol.Qry.QRY_PLINKY_PLONKY_SENSOR_REFERENCE)

    def send_cmd_stepper_target_start(self, name_operation, ip, name_device, reference, target_state, velocity_mhz, current_ma, timeout_ms):
        '''Helper function to send CMD_STEPPER_TARGET_START'''
        print(f"{name_operation}: CMD_STEPPER_TARGET_START (ref {reference}) to {name_device} ({ip})"
              f", target state {target_state.name}, velocity {velocity_mhz} mHz,"
              f" {current_ma} mA, timeout {timeout_ms} ms.")
        cmd = protocol.CmdMsg(protocol.Cmd.CMD_STEPPER_TARGET_START, reference, target_state, velocity_mhz, current_ma, timeout_ms)
        self.send_command(ip, cmd)

    def stand_rotate_clockwise(self, opposites_day=False):
        '''Helper function to rotate the stand'''
        # Clockwise is negative velocity
        velocity_mhz = -(1000 * 64 * 2)
        current_ma = 1000
        target_state = protocol.State.STATE_STAND_ROTATING_CLOCKWISE
        timeout_ms = 27000
        name_operation = "clockwise"
        if opposites_day:
            target_state = protocol.State.STATE_STAND_ROTATING_ANTICLOCKWISE
            name_operation = "anticlockwise"
            velocity_mhz = -velocity_mhz
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_STAND_INIT:
                self.send_cmd_stepper_target_start(f"rotate {name_operation}", ip, info["name"], info["reference"], target_state, velocity_mhz, current_ma, timeout_ms)
                info["reference"] = self.next_reference(info["reference"])

    def stand_stop(self):
        '''Helper function to stop the stand'''
        target_state = protocol.State.STATE_STAND_STOPPED
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_STAND_INIT:
                self.send_cmd_stepper_target_start(f"stop stand", ip, info["name"], info["reference"], target_state, 0, 0, 0)
                info["reference"] = self.next_reference(info["reference"])

    def lift_up(self, opposites_day=False):
        '''Helper function to raise and lower the lift'''
        # Up is positive velocity
        velocity_mhz = (1000 * 64 * 17)
        current_ma = 1000
        target_state = protocol.State.STATE_LIFT_STOPPED_UP
        timeout_ms = 27000
        name_operation = "raise"
        if opposites_day:
            target_state = protocol.State.STATE_LIFT_STOPPED_DOWN
            name_operation = "lower"
            velocity_mhz = -velocity_mhz
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_LIFT_INIT:
                self.send_cmd_stepper_target_start(f"{name_operation} lift", ip, info["name"], info["reference"], target_state, velocity_mhz, current_ma, timeout_ms)
                info["reference"] = self.next_reference(info["reference"])

    def plinky_plonky_play(self, opposites_day=False):
        '''Helper function to play or stop the plinky-plonky'''
        velocity_mhz = (1000 * 64 * 11)
        current_ma = 1000
        target_state = protocol.State.STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE
        timeout_ms = 35000
        name_operation = "play"
        if opposites_day:
            target_state = protocol.State.STATE_PLINKY_PLONKY_STOPPED_UNKNOWN
            name_operation = "stop"
            velocity_mhz = 0
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_PLINKY_PLONKY_INIT:
                self.send_cmd_stepper_target_start(f"{name_operation}", ip, info["name"], info["reference"], target_state, velocity_mhz, current_ma, timeout_ms)
                info["reference"] = self.next_reference(info["reference"])

    def door_open(self, index=-1, opposites_day=False):
        '''Helper function to open and close a door or all doors'''
        # Open is negative velocity
        velocity_mhz = -(1000 * 64 * 2)
        current_ma = 150
        target_state = protocol.State.STATE_DOOR_STOPPED_OPEN
        timeout_ms = 3000
        name_operation = "open"
        if opposites_day:
            target_state = protocol.State.STATE_DOOR_STOPPED_CLOSED
            timeout_ms = 2550
            name_operation = "close"
            velocity_mhz = -velocity_mhz
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                if index < 0 or index == info["index"]:
                    self.send_cmd_stepper_target_start(f"{name_operation} doors", ip, info["name"], info["reference"], target_state, velocity_mhz, current_ma, timeout_ms)
                    info["reference"] = self.next_reference(info["reference"])

    def reboot_all(self):
        '''Helper function to reboot all connected devices'''
        print("Requesting all devices to reboot.")
        for ip, info in self.devices.items():
            cmd = protocol.CmdMsg(protocol.Cmd.CMD_REBOOT, info["reference"], 0, 0, 0, 0)
            self.send_command(ip, cmd)
            info["reference"] = self.next_reference(info["reference"])

class EnumFormatter:
    """Helper class to format enum values with correct name ordering"""

    def __init__(self, header_path=None):
        if header_path is None:
            # Find protocol.h relative to this script
            script_dir = Path(__file__).resolve().parent
            header_path = script_dir.parent / 'protocol' / 'protocol.h'

        self.header_path = header_path
        self._load_enum_order()

    def _load_enum_order(self):
        """Parse the C header to get enum names in definition order"""
        with open(self.header_path, 'r') as f:
            content = f.read()

        # Find all enum definitions
        enum_pattern = r'typedef\s+enum\s*{([^}]+)}\s*(\w+)_t;'

        self.enum_order = {}
        for match in re.finditer(enum_pattern, content, re.DOTALL):
            enum_body = match.group(1)
            enum_name = match.group(2)

            # Parse enum entries in order
            entries = []
            for line in enum_body.split('\n'):
                line = line.strip()
                if line:
                    for entry in line.split(','):
                        entry = entry.strip()
                        if entry and not entry.startswith('//'):
                            entries.append(entry)

            # Store entries in order
            self.enum_order[enum_name] = entries

    def _get_name_from_order(self, enum_class, value):
        """
        Get the name for a value based on the order in the C header.
        Returns the LAST name defined for the value.
        """
        # Get the enum name from the class (e.g., 'ind' from Ind)
        enum_name = enum_class.__name__.lower()

        if enum_name not in self.enum_order:
            # Fallback: iterate through enum members to find last occurrence
            last_name = None
            for member in enum_class:
                if member.value == value:
                    last_name = member.name
            return last_name

        # Build mapping from value to list of names in definition order
        value_map = {}
        last_value = -1

        for entry in self.enum_order[enum_name]:
            if '=' in entry:
                name, expr = entry.split('=', 1)
                name = name.strip()
                expr = expr.strip()

                # Get the actual value from the enum
                try:
                    val = getattr(enum_class, name).value
                except AttributeError:
                    continue

                last_value = val
                if val not in value_map:
                    value_map[val] = []
                value_map[val].append(name)
            else:
                # Implicit value
                name = entry
                last_value += 1
                if last_value not in value_map:
                    value_map[last_value] = []
                value_map[last_value].append(name)

        # Return the last name for this value
        if value in value_map:
            return value_map[value][-1]
        return None

    def format(self, value, enum_classes=None, format_hex=True, strip_prefix=True):
        """
        Format a value with optional enum class(es).

        Args:
            value: The integer value to format
            enum_classes: Can be:
                - None: Just format as hex/decimal
                - A single enum class (e.g., protocol.Status)
                - A tuple/list of enum classes to try in order
                - The string "cmd_or_qry" to use the command/query pair
            format_hex: If True, show hex value; if False, show decimal
            strip_prefix: If True, strip enum prefixes from names
        """
        # Special case for command/query pair
        if enum_classes == "cmd_or_qry":
            # Try command enum
            name = self._get_name_from_order(protocol.Cmd, value)
            if name:
                if strip_prefix and name.startswith("CMD_"):
                    name = name[4:]
            else:
                # Try query enum
                name = self._get_name_from_order(protocol.Qry, value)
                if name and strip_prefix and name.startswith("QRY_"):
                    name = name[4:]

            if name:
                if format_hex:
                    return f"{name} (0x{value:x})"
                else:
                    return f"{name} ({value})"
            else:
                if format_hex:
                    return f"0x{value:x} (unknown)"
                else:
                    return f"{value} (unknown)"

        # Handle single enum class
        if isinstance(enum_classes, type):
            name = self._get_name_from_order(enum_classes, value)
            if name:
                if strip_prefix:
                    prefix = f"{enum_classes.__name__.upper()}_"
                    if name.startswith(prefix):
                        name = name[len(prefix):]
                if format_hex:
                    return f"{name} (0x{value:x})"
                else:
                    return f"{name} ({value})"
            else:
                if format_hex:
                    return f"0x{value:x} (unknown)"
                else:
                    return f"{value} (unknown)"

        # Handle list/tuple of enum classes
        if isinstance(enum_classes, (list, tuple)):
            for enum_class in enum_classes:
                name = self._get_name_from_order(enum_class, value)
                if name:
                    if strip_prefix:
                        prefix = f"{enum_class.__name__.upper()}_"
                        if name.startswith(prefix):
                            name = name[len(prefix):]
                    if format_hex:
                        return f"{name} (0x{value:x})"
                    else:
                        return f"{name} ({value})"

            # Not found in any enum
            if format_hex:
                return f"0x{value:x} (unknown)"
            else:
                return f"{value} (unknown)"

        # No enum class provided - just format the number
        if format_hex:
            return f"0x{value:x}"
        else:
            return f"{value}"

class Formatter:
    """
    Convenience class for formatting protocol values with clean method names.
    Uses the EnumFormatter internally for correct name ordering.
    """

    def __init__(self, enum_formatter=None):
        self.enum_formatter = enum_formatter or EnumFormatter()

    def status(self, value, format_hex=True, strip_prefix=True):
        """Format a status code"""
        return self.enum_formatter.format(value, protocol.Status, format_hex, strip_prefix)

    def state(self, value, format_hex=True, strip_prefix=True):
        """Format a state value"""
        return self.enum_formatter.format(value, protocol.State, format_hex, strip_prefix)

    def cmd(self, value, format_hex=True, strip_prefix=True):
        """Format a command code"""
        return self.enum_formatter.format(value, protocol.Cmd, format_hex, strip_prefix)

    def qry(self, value, format_hex=True, strip_prefix=True):
        """Format a query code"""
        return self.enum_formatter.format(value, protocol.Qry, format_hex, strip_prefix)

    def cmd_or_qry(self, value, format_hex=True, strip_prefix=True):
        """Format a value that could be either a command or query"""
        return self.enum_formatter.format(value, "cmd_or_qry", format_hex, strip_prefix)

    def ind(self, value, format_hex=True, strip_prefix=True):
        """Format an indication code"""
        return self.enum_formatter.format(value, protocol.Ind, format_hex, strip_prefix)

    def log_level(self, value, format_hex=True, strip_prefix=True):
        """Format a log level"""
        return self.enum_formatter.format(value, protocol.LogLevel, format_hex, strip_prefix)

    def value(self, value, enum_class=None, format_hex=True, strip_prefix=True):
        """Generic value formatter"""
        return self.enum_formatter.format(value, enum_class, format_hex, strip_prefix)

def main(port):
    # Create manager
    manager = MusicalBoxManager()

    # Create a formatter for pretty-prints
    formatter = Formatter()

    # Start the server (non-blocking)
    receiver_thread = manager.start(port)

    try:
        # Wait for all devices to connect and initialize
        if manager.wait_for_all_devices():

            # All devices ready, be the Musical Box
            print("=== Ready to play ===")
            #manager.reboot_all()
            #manager.plinky_plonky_play()
            #manager.stand_rotate_clockwise()
            #sleep(7)
            #manager.door_open(opposites_day=True)
            #manager.door_open()
            #sleep(5)
            #manager.lift_up(opposites_day=True)
            #manager.lift_up()

            # Process incoming messages
            while True:
                try:
                    ip, msg = manager.incoming_queue.get_nowait()
                    manager._process_message(ip, msg)
                except queue.Empty:
                    time.sleep(0.1)
        else:
            print("Failed to connect and initialize all devices")

    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        manager.stop()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=("A script to control all of the devices that form the musical box."),
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('-p', type=int, default=ESP32_PORT_DEFAULT, help=("the ESP32 control port, default "
                                                                         f"{ESP32_PORT_DEFAULT}."))
    args = parser.parse_args()

    main(args.p)

