#!/usr/bin/env python3

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
import argparse
from datetime import datetime

# The default listening port
LISTENING_PORT_DEFAULT = 5000

class ESP32Manager:
    def __init__(self):
        # Your known devices (fixed IPs)
        self.devices = {
            "10.10.3.10": {"name": "stand", "connected": False, "sock": None},
            "10.10.3.20": {"name": "lift", "connected": False, "sock": None},
            "10.10.3.30": {"name": "plinky-plonky", "connected": False, "sock": None},
            "10.10.3.40": {"name": "door 0", "connected": False, "sock": None},
            "10.10.3.41": {"name": "door 1", "connected": False, "sock": None},
            "10.10.3.42": {"name": "door 2", "connected": False, "sock": None},
            "10.10.3.43": {"name": "door 3", "connected": False, "sock": None},
            "10.10.3.44": {"name": "door 4", "connected": False, "sock": None},
            "10.10.3.45": {"name": "door 5", "connected": False, "sock": None},
        }
        
        # Single unified queue for incoming messages
        # Each message is (ip, data)
        self.incoming_queue = queue.Queue()
        
        # Socket management
        self.server_socket = None
        self.running = False
        
        # For select() polling
        self.socket_list = []
        self.device_sockets = {}  # Map socket -> ip
        
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
                            self.socket_list.append(client_sock)
                            self.device_sockets[client_sock] = client_ip
                            print(f"{self.devices[client_ip]['name']} connected from {client_ip}")
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
                                # Put in queue with device identifier
                                self.incoming_queue.put((ip, data))
                            else:
                                # Connection closed
                                self._handle_disconnect(sock, ip)
                        except:
                            # Error reading
                            self._handle_disconnect(sock, ip)
                
                # Handle exceptional conditions
                for sock in exceptional:
                    if sock in self.device_sockets:
                        ip = self.device_sockets[sock]
                        self._handle_disconnect(sock, ip)
                        
            except Exception as e:
                print(f"Error in receiver loop: {e}")
                time.sleep(1)
    
    def _handle_disconnect(self, sock, ip):
        """Clean up disconnected device"""
        if ip in self.devices:
            self.devices[ip]["connected"] = False
            self.devices[ip]["sock"] = None
            print(f"{self.devices[ip]['name']} disconnected")
        
        if sock in self.socket_list:
            self.socket_list.remove(sock)
        if sock in self.device_sockets:
            del self.device_sockets[sock]
        
        try:
            sock.close()
        except:
            pass
    
    def send_to_device(self, ip, data):
        """
        Send data to a specific device
        Returns True if sent, False if device not connected
        """
        if ip in self.devices and self.devices[ip]["connected"]:
            try:
                sock = self.devices[ip]["sock"]
                sock.send(data)
                return True
            except:
                print(f"Failed to send to {self.devices[ip]['name']}")
                return False
        return False
    
    def send_to_all(self, data):
        """Send data to all connected devices"""
        results = {}
        for ip, info in self.devices.items():
            if info["connected"]:
                results[ip] = self.send_to_device(ip, data)
        return results
    
    def wait_for_all_devices(self, timeout=None):
        """
        Block until all known devices are connected
        Returns True if all connected, False if timeout
        """
        print("Waiting for all devices to connect...")
        start_time = time.time()
        
        while self.running:
            all_connected = all(info["connected"] for info in self.devices.values())
            
            if all_connected:
                print("All devices connected!")
                return True
            
            if timeout and (time.time() - start_time) > timeout:
                print("Timeout waiting for devices")
                return False
            
            time.sleep(1)
    
    def get_connected_devices(self):
        """Return list of currently connected device IPs"""
        return [ip for ip, info in self.devices.items() if info["connected"]]
    
    def is_device_connected(self, ip):
        """Check if a specific device is connected"""
        return ip in self.devices and self.devices[ip]["connected"]
    
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

# ===== USAGE EXAMPLE =====

def main(port):
    # Create manager
    manager = ESP32Manager()
    
    # Start the server (non-blocking)
    receiver_thread = manager.start(port)
    
    try:
        # Wait for all devices to connect (no timeout)
        if manager.wait_for_all_devices():
            
            # All devices connected - do your main logic here
            print("=== ALL SYSTEMS GO ===")
            
            # Example: Process incoming messages
            while True:
                try:
                    # Check for incoming messages (non-blocking)
                    ip, data = manager.incoming_queue.get_nowait()
                    device_name = manager.devices[ip]["name"]
                    print(f"From {device_name}: {data}")
                    
                    # Echo back as example
                    manager.send_to_device(ip, b"ACK")
                    
                except queue.Empty:
                    # No messages, do other work
                    time.sleep(0.1)
                    
        else:
            print("Failed to connect all devices")
            
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

