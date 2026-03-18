!/usr/bin/env python

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

# All written by DeepSeek.

import socket
import syslog
import sys
import os
import logging
import struct
from systemd import journal
from pathlib import Path

# Add the protocol directory to Python path
# Get the directory where THIS script is located
script_dir = Path(__file__).resolve().parent

# Navigate up to the common directory where protocol.h and protocol.py live
protocol_dir = script_dir.parent / 'protocol'
sys.path.insert(0, str(protocol_dir))

from protocol import LogMsg, LogLevel

class ESP32LogServer:
    def __init__(self, port=5001):
        self.port = port
        self.server = None
        self.running = False

        # Set up logging to journal
        self.logger = logging.getLogger('esp32_logger')
        self.logger.propagate = False
        self.logger.addHandler(journal.JournalHandler(SYSLOG_IDENTIFIER='esp32-device'))
        self.logger.setLevel(logging.DEBUG)

    def start(self):
        """Start the log server"""
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(('0.0.0.0', self.port))
        self.server.listen(5)

        print(f"ESP32 log server listening on port {self.port}")
        print("[view messages with \"journalctl -t esp32-device\"]")
        self.running = True

        while self.running:
            try:
                client, addr = self.server.accept()
                print(f"ESP32 connection from {addr[0]}:{addr[1]}")
                self.handle_client(client, addr)
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"Error accepting connection: {e}")

    def handle_client(self, client, addr):
        """Handle a connected ESP32 client"""
        buffer = b''
        expected_size = LogMsg.SIZE
        client.settimeout(1.0)  # Add timeout to prevent blocking

        while self.running:
            try:
                data = client.recv(expected_size)
                if not data:
                    print(f"Client {addr[0]} disconnected")
                    break

                # Add to buffer
                buffer += data

                # Process complete messages
                while len(buffer) >= expected_size:
                    msg_data = buffer[:expected_size]
                    buffer = buffer[expected_size:]

                    try:
                        # Debug: print raw data
                        #print(f"DEBUG: Received {len(msg_data)} bytes: {msg_data.hex()}")

                        # Unpack the message using the protocol module
                        log_msg = LogMsg.unpack(msg_data)

                        # Verify it's a LogMsg object
                        if log_msg.magic != LogMsg.MAGIC:
                            print(f"WARNING: Invalid magic byte: {log_msg.magic:#x}")
                            continue

                        self.forward_to_journal(log_msg, addr)

                    except struct.error as e:
                        print(f"Struct unpacking error: {e}")
                    except ValueError as e:
                        print(f"Value error unpacking message: {e}")
                    except Exception as e:
                        print(f"Error parsing log message: {e}")
                        print(f"Raw data (hex): {msg_data.hex()}")

            except socket.timeout:
                continue
            except ConnectionResetError:
                print(f"Client {addr[0]} reset connection")
                break
            except Exception as e:
                print(f"Error receiving data: {e}")
                break

        client.close()
        print(f"ESP32 disconnected from {addr[0]}")

    def forward_to_journal(self, log_msg, addr):
        """Forward ESP32 log message to systemd journal with IP prepended"""

        # Ensure log_msg is a LogMsg object
        if not hasattr(log_msg, 'level') or not hasattr(log_msg, 'message'):
            print(f"ERROR: Invalid log message object: {type(log_msg)}")
            return

        # Map your protocol log levels to logging levels
        priority_map = {
            LogLevel.LOG_DEBUG: logging.DEBUG,
            LogLevel.LOG_INFO: logging.INFO,
            LogLevel.LOG_WARN: logging.WARNING,
            LogLevel.LOG_ERROR: logging.ERROR,
        }

        # Get Python logging level
        level = priority_map.get(log_msg.level, logging.INFO)

        # Clean the message (remove null bytes and strip whitespace)
        clean_message = log_msg.message.rstrip('\x00').strip()

        # Skip empty messages
        if not clean_message:
            return

        # Prepend the IP address to the message
        ip_address = addr[0]
        enhanced_message = f"[{ip_address}] {clean_message}"

        # Add structured fields for better journalctl querying
        extra = {
            'ESP32_LEVEL': log_msg.level,
            'ESP32_ADDR': ip_address,
            'ESP32_PORT': addr[1],
            'ESP32_LEVEL_NAME': LogLevel(log_msg.level).name if log_msg.level in LogLevel._value2member_map_ else 'UNKNOWN',
            'CODE_MODULE': 'esp32_log_handler',
        }

        # Log to journal with IP in message and structured fields
        self.logger.log(level, enhanced_message, extra=extra)

        # Also print to console for debugging
        print(f"LOG: {enhanced_message}")

    def stop(self):
        """Stop the server"""
        self.running = False
        if self.server:
            self.server.close()

if __name__ == "__main__":
    # Configure logging to also show debug info
    logging.basicConfig(level=logging.DEBUG)

    server = ESP32LogServer(port=5001)
    try:
        print("Starting ESP32 log server. Press Ctrl+C to stop.")
        server.start()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
