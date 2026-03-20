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
LISTENING_PORT_DEFAULT = 5000

class ESP32Manager:
    def __init__(self):
        # Your known devices (fixed IPs)
        self.devices = {
            "10.10.3.10": {"name": "stand", "init": protocol.Cmd.CMD_STAND_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0},
            "10.10.3.20": {"name": "lift", "init": protocol.Cmd.CMD_LIFT_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0},
            "10.10.3.30": {"name": "plinky-plonky", "init": protocol.Cmd.CMD_PLINKY_PLONKY_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0},
            # Doors have index values also
            "10.10.3.40": {"name": "door 0", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 0},
            "10.10.3.41": {"name": "door 1", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 1},
            "10.10.3.42": {"name": "door 2", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 2},
            "10.10.3.43": {"name": "door 3", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 3},
            "10.10.3.44": {"name": "door 4", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 4},
            "10.10.3.45": {"name": "door 5", "init": protocol.Cmd.CMD_DOOR_INIT, "connected": False, "sock": None, "init_done": False, "reference": 0, "index": 5},
        }

        # Single unified queue for incoming messages (RspMsg and IndMsg only)
        # Each message is (ip, msg_obj)
        self.incoming_queue = queue.Queue()

        # Socket management
        self.server_socket = None
        self.running = False

        # For select() polling
        self.socket_list = []
        self.device_sockets = {}  # Map socket -> ip

        # Partial message buffer for each socket (in case we get fragmented reads)
        self.socket_buffers = {}  # Map socket -> bytes

        # Track pending init responses
        self.pending_inits = {}  # Map socket -> (ip, reference, timestamp)

    def start(self, port=5000):
        """Start the server and wait for connections"""
        self.running = True

        # Create server socket
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind(('0.0.0.0', port))
        self.server_socket.listen(len(self.devices))

        # Add server socket to list for select
        self.socket_list = [self.server_socket]

        print(f"Listening for ESP32s on port {port}")
        print("Known devices:")
        for ip, info in self.devices.items():
            print(f"  - {info['name']} ({ip})")
        print()

        # Start receiver thread
        receiver_thread = threading.Thread(target=self._receiver_loop)
        receiver_thread.daemon = True
        receiver_thread.start()

        return receiver_thread

    def _parse_message(self, data: bytes) -> Optional[Union[protocol.RspMsg, protocol.IndMsg]]:
        """
        Parse incoming data based on magic byte
        Returns the appropriate message object or None if invalid
        """
        if len(data) < 1:
            return None

        magic = data[0]

        try:
            if magic == protocol.PROTOCOL_MAGIC_RSP:
                return protocol.RspMsg.unpack(data)
            elif magic == protocol.PROTOCOL_MAGIC_IND:
                return protocol.IndMsg.unpack(data)
            else:
                # Log messages and unknown magics are ignored
                return None
        except Exception as e:
            print(f"Error parsing message: {e}")
            return None

    def _handle_init_response(self, sock, ip, msg):
        """Handle response to init command"""
        if sock not in self.pending_inits:
            return False

        expected_ref, expected_cmd, timestamp = self.pending_inits[sock]  # Store expected command too

        # Check if this response matches our pending init
        if msg.reference == expected_ref and msg.cmd_or_qry == expected_cmd:
            if msg.status == protocol.Status.STATUS_OK:
                print(f"✓ {self.devices[ip]['name']} initialized successfully")
                self.devices[ip]["init_done"] = True
            else:
                print(f"✗ {self.devices[ip]['name']} initialization failed with status {protocol.Status(msg.status).name}")

            # Remove from pending
            del self.pending_inits[sock]
            return True

        return False

    def _receiver_loop(self):
        """Main loop using select() to handle all sockets"""
        while self.running:
            try:
                # Check which sockets have data
                readable, _, exceptional = select.select(self.socket_list, [], [], 1.0)

                for sock in readable:
                    if sock is self.server_socket:
                        # New connection
                        client_sock, addr = self.server_socket.accept()
                        client_ip = addr[0]

                        if client_ip in self.devices:
                            # Known device - accept
                            self.devices[client_ip]["connected"] = True
                            self.devices[client_ip]["sock"] = client_sock
                            self.devices[client_ip]["init_done"] = False  # Reset init flag
                            self.socket_list.append(client_sock)
                            self.device_sockets[client_sock] = client_ip
                            self.socket_buffers[client_sock] = b""  # Initialize buffer
                            print(f"{self.devices[client_ip]['name']} connected from {client_ip}")

                            # Send init command
                            cmd = protocol.CmdMsg(self.devices[client_ip]["init"], self.devices[client_ip]["reference"], 0, 0, 0, 0)

                            # Track this pending init with both reference and command
                            self.pending_inits[client_sock] = (self.devices[client_ip]["reference"], self.devices[client_ip]["init"], time.time())

                            # Send the command
                            protocol.send_message(client_sock, cmd)
                            self.devices[client_ip]["reference"] = self.next_reference(self.devices[client_ip]["reference"])
                            print(f"  Sent init command (ref={self.devices[client_ip]["reference"]}) to {self.devices[client_ip]['name']}")
                        else:
                            # Unknown device - reject
                            print(f"Rejected unknown device from {client_ip}")
                            client_sock.close()

                    elif sock in self.device_sockets:
                        # Data from known device
                        ip = self.device_sockets[sock]
                        try:
                            data = sock.recv(1024)
                            if data:
                                #print(f"{ip}: received {len(data)} byte(s): ", end='')
                                #for item in data:
                                #    print(f"{item:02x}", end='')
                                #print("")
                                # Add to buffer and try to parse complete messages
                                self.socket_buffers[sock] += data

                                # Try to parse messages from buffer
                                while True:
                                    if len(self.socket_buffers[sock]) < 1:
                                        break

                                    # Determine expected message size from magic byte
                                    magic = self.socket_buffers[sock][0]
                                    msg_size = None

                                    if magic == protocol.PROTOCOL_MAGIC_RSP:
                                        msg_size = protocol.RspMsg.SIZE
                                    elif magic == protocol.PROTOCOL_MAGIC_IND:
                                        msg_size = protocol.IndMsg.SIZE
                                    else:
                                        # Unknown magic - discard first byte and continue
                                        # (silently ignores log messages and invalid data)
                                        self.socket_buffers[sock] = self.socket_buffers[sock][1:]
                                        continue

                                    # Check if we have a complete message
                                    if len(self.socket_buffers[sock]) >= msg_size:
                                        # Extract and parse message
                                        msg_data = self.socket_buffers[sock][:msg_size]
                                        self.socket_buffers[sock] = self.socket_buffers[sock][msg_size:]

                                        msg = self._parse_message(msg_data)
                                        if msg:
                                            # Check if this is a response to a pending init
                                            if sock in self.pending_inits and isinstance(msg, protocol.RspMsg):
                                                if self._handle_init_response(sock, ip, msg):
                                                    # This response was consumed by init handling
                                                    continue

                                            # Not an init response, add to queue
                                            self.incoming_queue.put((ip, msg))
                                    else:
                                        # Wait for more data
                                        break
                            else:
                                # Connection closed
                                self._handle_disconnect(sock, ip)
                        except Exception as e:
                            print(f"Error reading from {ip}: {e}")
                            self._handle_disconnect(sock, ip)

                # Handle exceptional conditions
                for sock in exceptional:
                    if sock in self.device_sockets:
                        ip = self.device_sockets[sock]
                        self._handle_disconnect(sock, ip)

                # Clean up stale pending inits (timeout after 5 seconds)
                current_time = time.time()
                stale_socks = []
                for sock, (ref, cmd, timestamp) in self.pending_inits.items():  # Unpack all 3 values
                    if current_time - timestamp > 5.0:
                        ip = self.device_sockets.get(sock, "unknown")
                        print(f"Warning: Init response timeout for {ip}")
                        stale_socks.append(sock)

                for sock in stale_socks:
                    if sock in self.pending_inits:
                        del self.pending_inits[sock]

            except Exception as e:
                print(f"Error in receiver loop: {e}")
                time.sleep(1)

    def _handle_disconnect(self, sock, ip):
        """Clean up disconnected device"""
        if ip in self.devices:
            self.devices[ip]["connected"] = False
            self.devices[ip]["sock"] = None
            self.devices[ip]["init_done"] = False
            print(f"{self.devices[ip]['name']} disconnected")

        # Clean up pending init
        if sock in self.pending_inits:
            del self.pending_inits[sock]

        if sock in self.socket_list:
            self.socket_list.remove(sock)
        if sock in self.device_sockets:
            del self.device_sockets[sock]
        if sock in self.socket_buffers:
            del self.socket_buffers[sock]

        try:
            sock.close()
        except:
            pass

    def send_command(self, ip: str, command: protocol.CmdMsg) -> bool:
        """
        Send a command message to a specific device
        """
        if ip in self.devices and self.devices[ip]["connected"]:
            # Check if device is initialized (unless this IS the init command)
            if command.command not in [info["init"] for info in self.devices.values()]:
                if not self.devices[ip]["init_done"]:
                    print(f"Warning: {self.devices[ip]['name']} not yet initialized")
                    return False

            try:
                sock = self.devices[ip]["sock"]
                return protocol.send_message(sock, command)
            except Exception as e:
                print(f"Failed to send to {self.devices[ip]['name']}: {e}")
                return False
        return False

    def next_reference(self, reference) -> int:
        reference += 1
        if reference > 255:
            reference = 0
        return reference

    def send_query(self, ip: str, query: protocol.QryMsg) -> bool:
        """
        Send a query message to a specific device
        """
        if ip in self.devices and self.devices[ip]["connected"]:
            # Check if device is initialized
            if not self.devices[ip]["init_done"]:
                print(f"Warning: {self.devices[ip]['name']} not yet initialized")
                return False

            try:
                sock = self.devices[ip]["sock"]
                return protocol.send_message(sock, query)
            except Exception as e:
                print(f"Failed to send to {self.devices[ip]['name']}: {e}")
                return False
        return False

    def send_command_to_all(self, command: protocol.CmdMsg) -> Dict[str, bool]:
        """Send command to all connected devices"""
        results = {}
        for ip, info in self.devices.items():
            if info["connected"] and info["init_done"]:
                results[ip] = self.send_command(ip, command)
        return results

    def send_query_to_all(self, query: protocol.QryMsg) -> Dict[str, bool]:
        """Send query to all connected devices"""
        results = {}
        for ip, info in self.devices.items():
            if info["connected"] and info["init_done"]:
                results[ip] = self.send_query(ip, query)
        return results

    def wait_for_all_devices(self, timeout=None):
        """Block until all known devices are connected AND initialized"""
        print("Waiting for all devices to connect and initialize...")
        start_time = time.time()
        last_print_time = 0

        CLEAR_LINE = "\033[2K"
        CURSOR_START = "\033[0G"

        while self.running:
            all_connected = all(info["connected"] for info in self.devices.values())
            all_initialized = all(info["init_done"] for info in self.devices.values())

            if all_connected and all_initialized:
                elapsed = time.time() - start_time
                print(f"\nAll devices connected and initialized! (took {elapsed:.1f}s)")
                return True

            elapsed = time.time() - start_time
            if timeout and elapsed > timeout:
                self._print_waiting_status(True, elapsed)
                print("\nTimeout waiting for devices")
                return False

            current_time = time.time()
            if current_time - last_print_time >= 5.0:
                self._print_waiting_status(False, elapsed)
                last_print_time = current_time

            time.sleep(1)

        return False

    def _print_waiting_status(self, final=False, elapsed=0):
        """Print the current waiting status on a single line"""
        waiting_connect = []
        waiting_init = []

        for ip, info in self.devices.items():
            if not info["connected"]:
                waiting_connect.append(f"{info['name']} [{ip}]")
            elif not info["init_done"]:
                waiting_init.append(f"{info['name']} [{ip}]")

        CLEAR_LINE = "\033[2K"
        CURSOR_START = "\033[0G"

        if waiting_connect or waiting_init:
            status_parts = []
            if waiting_connect:
                status_parts.append(f"{', '.join(waiting_connect)}")
            if waiting_init:
                status_parts.append(f"initializing {', '.join(waiting_init)}")

            status = f"⏳ [{elapsed:.0f}s] Waiting for: {'; '.join(status_parts)}"

            if final:
                print(f"{CLEAR_LINE}{CURSOR_START}{status}")
            else:
                print(f"{CLEAR_LINE}{CURSOR_START}{status}", end='', flush=True)

    def get_connected_devices(self):
        """Return list of currently connected device IPs"""
        return [ip for ip, info in self.devices.items() if info["connected"]]

    def is_device_connected(self, ip):
        """Check if a specific device is connected"""
        return ip in self.devices and self.devices[ip]["connected"]

    def is_device_initialized(self, ip):
        """Check if a specific device is initialized"""
        return ip in self.devices and self.devices[ip]["init_done"]

    def stop(self):
        """Clean shutdown"""
        self.running = False

        # Close all sockets
        for sock in self.socket_list:
            try:
                sock.close()
            except:
                pass

        print("Manager stopped")

class MusicalBoxManager(ESP32Manager):
    """Specialized manager for the Musical Box stuff"""
    def __init__(self):
        # Call parent constructor
        super().__init__()

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
        timeout_ms = 30000
        name_operation = "clockwise"
        if opposites_day:
            target_state = protocol.State.STATE_STAND_ROTATING_ANTICLOCKWISE
            name_operation = "anticlockwise"
            velocity_mhz = -velocity_mhz
        for ip, info in self.devices.items():
            if info["init"] == protocol.Cmd.CMD_STAND_INIT:
                self.send_cmd_stepper_target_start(f"rotate {name_operation}", ip, info["name"], info["reference"], target_state, velocity_mhz, current_ma, timeout_ms)
                info["reference"] = self.next_reference(info["reference"])

    def lift_up(self, opposites_day=False):
        '''Helper function to raise and lower the lift'''
        # Up is positive velocity
        velocity_mhz = (1000 * 64 * 10)
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
        velocity_mhz = (1000 * 64 * 7 * 2)
        current_ma = 1000
        target_state = protocol.State.STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE
        timeout_ms = 30000
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
            #manager.lift_up()
            manager.door_open(opposites_day=True)
            #manager.door_open()

            # Process incoming messages
            while True:
                try:
                    # Check for incoming messages (non-blocking)
                    ip, msg = manager.incoming_queue.get_nowait()
                    device_name = manager.devices[ip]["name"]

                    if isinstance(msg, protocol.RspMsg):
                        print(f"{device_name}: response to {formatter.cmd_or_qry(msg.cmd_or_qry)}, reference {msg.reference}: status {formatter.status(msg.status)} (value {msg.value})")

                    elif isinstance(msg, protocol.IndMsg):
                        print(f"{device_name}: indication {formatter.ind(msg.ind)} (value {msg.value})")

                except queue.Empty:
                    # No messages, do other work
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
    parser.add_argument('-p', type=int, default=LISTENING_PORT_DEFAULT, help=("the listening port, default "
                                                                             f"{LISTENING_PORT_DEFAULT}."))
    args = parser.parse_args()

    main(args.p)

