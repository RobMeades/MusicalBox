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

#  Utterly written by DeepSeek; very impressive indeed.

import asyncio
import json
import threading
import queue
import time
import argparse
from datetime import datetime
from typing import Optional
from collections import deque

from aiohttp import web
import aiohttp_jinja2
import jinja2

# Import MusicalBoxManager
from musical_box_manager import MusicalBoxManager, Formatter
import protocol

# The default HTTP port
HTTP_PORT_DEFAULT = 8080

# The ESP32 control port
ESP32_PORT_DEFAULT = 5000

# Max log entries to keep
MAX_LOG_ENTRIES = 500


class WebControlInterface:
    """Web-based control interface for the Musical Box"""

    def __init__(self, manager: MusicalBoxManager, host='0.0.0.0', port=HTTP_PORT_DEFAULT):
        self.manager = manager
        self.manager.set_log_callback(self._log_message)
        self.host = host
        self.port = port
        self.app = None
        self.runner = None
        self.running = False

        # Auto-run settings (in-memory only)
        self.auto_run_enabled = False
        self.auto_run_interval = 120  # seconds (2 minutes default)
        self.auto_run_task = None
        self.auto_run_sequence = "open_close"

        # Current operation status
        self.current_operation = None
        self.operation_start_time = None
        self.operation_status = "idle"  # idle, running, waiting, error

        # Formatter for pretty status
        self.formatter = Formatter()

        # Log storage (deque for automatic max size)
        self.log_entries = deque(maxlen=MAX_LOG_ENTRIES)

        # SSE clients
        self.sse_clients = set()

        # Store the HTML template as a string
        self.html_template = self._get_html_template()

        # Track if we're already notifying
        self._status_version = 0
        self._log_version = 0

    async def start(self):
        """Start the web server"""
        self.app = web.Application()

        # Routes
        self.app.router.add_get('/', self.handle_index)
        self.app.router.add_get('/api/status', self.handle_api_status)
        self.app.router.add_get('/api/status/stream', self.handle_status_stream)
        self.app.router.add_get('/api/logs/stream', self.handle_logs_stream)
        self.app.router.add_post('/api/command', self.handle_api_command)
        self.app.router.add_post('/api/settings', self.handle_api_settings)
        self.app.router.add_get('/api/logs', self.handle_api_logs)
        self.app.router.add_post('/api/logs/clear', self.handle_api_logs_clear)

        # Set up a queue to receive logs from the manager
        self.log_queue = queue.Queue()

        # Start a task to collect logs from the manager's print statements
        self._redirect_logs()

        self.runner = web.AppRunner(self.app)
        await self.runner.setup()

        site = web.TCPSite(self.runner, self.host, self.port)
        await site.start()

        self.running = True
        print(f"\nWeb interface running at http://{self.host}:{self.port}")

        # Start background tasks for SSE
        asyncio.create_task(self._broadcast_status())
        asyncio.create_task(self._broadcast_logs())

    def _redirect_logs(self):
        """Redirect print statements to our log storage"""
        # Monkey-patch print to capture logs
        self.original_print = print

        def captured_print(*args, **kwargs):
            message = ' '.join(str(arg) for arg in args)
            timestamp = datetime.now().strftime('%H:%M:%S')
            log_entry = f"[{timestamp}] {message}"
            self.log_entries.append(log_entry)
            self._log_version += 1
            self.original_print(*args, **kwargs)

        import builtins
        builtins.print = captured_print

    def _restore_print(self):
        """Restore original print function"""
        import builtins
        builtins.print = self.original_print

    async def handle_index(self, request):
        """Serve the main HTML page from embedded template"""
        return web.Response(text=self.html_template, content_type='text/html')

    async def handle_api_status(self, request):
        """Return current system status as JSON"""
        try:
            status = self._get_system_status()
            return web.json_response(status)
        except Exception as e:
            print(f"ERROR in handle_api_status: {e}")
            import traceback
            traceback.print_exc()
            return web.json_response({'error': str(e)}, status=500)

    async def handle_status_stream(self, request):
        """Server-Sent Events stream for status updates"""
        response = web.StreamResponse(
            status=200,
            headers={
                'Content-Type': 'text/event-stream',
                'Cache-Control': 'no-cache',
                'Connection': 'keep-alive',
            }
        )
        await response.prepare(request)

        client_id = id(response)
        self.sse_clients.add(client_id)

        try:
            last_version = 0
            while self.running:
                try:
                    if self._status_version > last_version:
                        last_version = self._status_version
                        status = self._get_system_status()
                        await response.write(f"data: {json.dumps(status)}\n\n".encode())
                    await asyncio.sleep(0.5)
                except ConnectionResetError:
                    # Client disconnected
                    print(f"DEBUG: Status client {client_id} disconnected")
                    break
                except RuntimeError as e:
                    if "Cannot write to closing transport" in str(e):
                        print(f"DEBUG: Status client {client_id} closed connection")
                        break
                    raise
        except Exception as e:
            print(f"DEBUG: Status stream error: {e}")
        finally:
            self.sse_clients.discard(client_id)
            print("DEBUG: Status stream ended")

        return response

    async def handle_logs_stream(self, request):
        """Server-Sent Events stream for log updates (incremental)"""
        response = web.StreamResponse(
            status=200,
            headers={
                'Content-Type': 'text/event-stream',
                'Cache-Control': 'no-cache',
                'Connection': 'keep-alive',
            }
        )
        await response.prepare(request)
        
        client_id = id(response)
        self.sse_clients.add(client_id)
        
        # Track the last count for this client
        last_count = 0
        
        try:
            while self.running:
                try:
                    current_count = len(self.log_entries)
                    
                    if last_count == 0 or current_count != last_count:
                        if current_count == 0:
                            await response.write(f"data: {json.dumps([])}\n\n".encode())
                        else:
                            new_logs = list(self.log_entries)[last_count:]
                            await response.write(f"data: {json.dumps(new_logs)}\n\n".encode())
                        last_count = current_count
                    
                    await asyncio.sleep(0.5)
                except ConnectionResetError:
                    # Client disconnected
                    print(f"DEBUG: Client {client_id} disconnected")
                    break
                except RuntimeError as e:
                    if "Cannot write to closing transport" in str(e):
                        print(f"DEBUG: Client {client_id} closed connection")
                        break
                    raise
        except Exception as e:
            print(f"DEBUG: Logs stream error: {e}")
        finally:
            self.sse_clients.discard(client_id)
        
        return response

    async def _broadcast_status(self):
        """Broadcast status updates to all SSE clients"""
        while self.running:
            try:
                self._status_version += 1
            except Exception:
                pass
            await asyncio.sleep(2)  # Update every 2 seconds

    async def _broadcast_logs(self):
        """Broadcast log updates to all SSE clients"""
        while self.running:
            try:
                self._log_version += 1
            except Exception:
                pass
            await asyncio.sleep(1)  # Update every 1 second

    async def handle_api_command(self, request):
        """Handle command requests"""
        data = await request.json()
        command = data.get('command')
        params = data.get('params', {})

        result = await self._execute_command(command, params)

        # Also log the result to the debug window
        if result['status'] == 'error':
            self._log_message(f"ERROR: Command '{command}' failed - {result['message']}")
        else:
            self._log_message(f"Command '{command}': {result['message']}")

        # Trigger immediate status update
        self._status_version += 1

        return web.json_response(result)

    async def handle_api_settings(self, request):
        """Handle settings updates (in-memory only)"""
        data = await request.json()

        if 'auto_run_enabled' in data:
            self.auto_run_enabled = data['auto_run_enabled']
            if self.auto_run_enabled and not self.auto_run_task:
                self._start_auto_run()
            elif not self.auto_run_enabled and self.auto_run_task:
                self._stop_auto_run()

        if 'auto_run_interval' in data:
            self.auto_run_interval = data['auto_run_interval']
            if self.auto_run_enabled:
                self._restart_auto_run()

        if 'auto_run_sequence' in data:
            self.auto_run_sequence = data['auto_run_sequence']

        # Trigger immediate status update
        self._status_version += 1

        return web.json_response({'status': 'ok', 'settings': {
            'auto_run_enabled': self.auto_run_enabled,
            'auto_run_interval': self.auto_run_interval,
            'auto_run_sequence': self.auto_run_sequence
        }})

    async def handle_api_logs(self, request):
        """Return recent logs"""
        logs = list(self.log_entries)
        return web.json_response({'logs': logs})

    async def handle_api_logs_clear(self, request):
        """Clear the log buffer"""
        self.log_entries.clear()
        self._log_version += 1
        return web.json_response({'status': 'ok'})

    async def _execute_command(self, command, params):
        """Execute a command and return result"""
        result = {'status': 'ok', 'message': ''}

        try:
            if command == 'open_sequence':
                asyncio.create_task(self._run_open_sequence())
                result['message'] = 'Open sequence started'

            elif command == 'close_sequence':
                asyncio.create_task(self._run_close_sequence())
                result['message'] = 'Close sequence started'

            elif command == 'stand_rotate':
                direction = params.get('direction', 'clockwise')
                self.manager.stand_rotate_clockwise(opposites_day=(direction == 'anticlockwise'))
                result['message'] = f'Stand rotating {direction}'

            elif command == 'stand_stop':
                # Stop stand - set target state to stopped
                self.manager.stand_stop()  # This doesn't actually stop
                result['message'] = 'Stand stop requested'

            elif command == 'plinky_plonky_play':
                self.manager.plinky_plonky_play()
                result['message'] = 'Plinky-plonky playing'

            elif command == 'plinky_plonky_stop':
                self.manager.plinky_plonky_play(opposites_day=True)
                result['message'] = 'Plinky-plonky stopped'

            elif command == 'door_open':
                index = params.get('index', -1)
                self.manager.door_open(index=index)
                result['message'] = f'Door {index if index >= 0 else "all"} opening'

            elif command == 'door_close':
                index = params.get('index', -1)
                self.manager.door_open(index=index, opposites_day=True)
                result['message'] = f'Door {index if index >= 0 else "all"} closing'

            elif command == 'lift_up':
                # Add debugging to see what's happening
                self._log_message("Checking if doors are open...")

                # First, log current door states
                door_state_dict = self.manager.door_state()
                self._log_message(f"Current door states: {str(door_state_dict)}")

                # Also log which devices are doors
                for ip, info in self.manager.devices.items():
                    if self.manager.is_door(ip):
                        self._log_message(f"Door device {ip}: initialised={info['initialised']}, state={self.manager.door_state(ip)}")

                # Check if doors are open
                doors_open = await self._check_doors_open()
                self._log_message(f"Doors open check result: {doors_open}")

                if doors_open:
                    self.manager.lift_up(opposites_day=False)
                    result['message'] = 'Lift raising'
                    self._log_message("Lift raising command sent")
                else:
                    result['status'] = 'error'
                    result['message'] = 'Cannot raise lift: doors are not open (or door states unknown)'
                    self._log_message("Lift raise blocked: doors not open")

            elif command == 'lift_down':
                self.manager.lift_up(opposites_day=True)
                result['message'] = 'Lift lowering'

            elif command == 'reboot_all':
                self.manager.reboot_all()
                result['message'] = 'Rebooting all devices'

            else:
                result['status'] = 'error'
                result['message'] = f'Unknown command: {command}'

        except Exception as e:
            result['status'] = 'error'
            result['message'] = str(e)
            self._log_message(f"Error executing {command}: {e}")

        return result

    def _log_message(self, message):
        """Add a message to the log buffer"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        self.log_entries.append(f"[{timestamp}] {message}")
        self._log_version += 1

    def _query_door_sensors(self):
        door_count = 0
        self._log_message("Querying door sensors...")
        for ip, info in self.manager.devices.items():
            if self.manager.is_door(ip):
                door_count += 1
                self.manager.query_door_sensor(ip)
        return door_count

    def _count_and_print_door_states(self):
        open_count = 0
        self._log_message("Current door states:")
        for ip, info in self.manager.devices.items():
            if self.manager.is_door(ip):
                state = self.manager.door_state(ip)
                sensor = self.manager._door_sensors.get(ip, {}).get('open', False)
                self._log_message(f"  Door {ip}: state={state}, sensor={sensor}")
                if sensor:
                    open_count += 1
        return open_count

    async def _run_open_sequence(self):
        """Run the complete open sequence"""
        self.operation_status = "running"
        self.current_operation = "open_sequence"
        self.operation_start_time = datetime.now()
        self._status_version += 1
        
        try:
            # Query door sensors (to make sure none are already open)
            door_count = self._query_door_sensors()
            
            # Wait for responses
            await asyncio.sleep(2)

            # Check and log the current door states
            open_count = self._count_and_print_door_states()

            if open_count == 0:        
                self._log_message("Starting music and rotation...")
                self.manager.plinky_plonky_play()
                self.manager.stand_rotate_clockwise()

                self._log_message("Waiting for 7 seconds...")
                await asyncio.sleep(7)

                self._log_message("Opening doors...")
                self.manager.door_open()

                self._log_message("Waiting 5 seconds...")
                await asyncio.sleep(5)

                # Log the current door states
                open_count = self._count_and_print_door_states()
                if open_count < door_count:
                    self._log_message(f"Only {open_count} out of {door_count} door sensors"
                                      " indicate open but sensors can be affected by"
                                      " ambient light so raising lift anyway")
                    
                # Raise lift
                self._log_message("Raising lift...")
                self.manager.lift_up()
            else:
                self._log_message("Not starting the open sequence as there are"
                                  f" {open_count} door(s) already open")

            self._log_message("Waiting for music to stop...")
            wait_guard_seconds = 0
            self.manager.query_plinky_plonky_state()
            # Wait for response
            await asyncio.sleep(1)
            while (not self.manager.is_plinky_plonky_at_reference()) and wait_guard_seconds < 60:
                self.manager.query_plinky_plonky_state()
                await asyncio.sleep(1)
                wait_guard_seconds += 1

            if wait_guard_seconds >= 60:
                 self._log_message("Plinky-plonky did not signal reference position"
                                   f" within {wait_guard_seconds} second(s), considering"
                                   f" the sequence completed anyway.")
            self.operation_status = "completed"
            self._log_message("Open sequence completed")
            
        except Exception as e:
            self.operation_status = "error"
            self._log_message(f"Open sequence error: {e}")
        finally:
            self._status_version += 1

    async def _run_close_sequence(self):
        """Run the complete close sequence"""
        self.operation_status = "running"
        self.current_operation = "close_sequence"
        self.operation_start_time = datetime.now()
        self._status_version += 1

        try:
            self._log_message("Starting music and rotation...")
            self.manager.plinky_plonky_play()
            self.manager.stand_rotate_clockwise()

            self._log_message("Waiting for 7 seconds...")
            await asyncio.sleep(7)

            # Lower lift
            self._log_message("Lowering lift...")
            self.manager.lift_up(opposites_day=True)
            lift_down_wait_seconds = 15
            self.manager.query_lift_state()
            # Wait for response
            await asyncio.sleep(1)
            while not self.manager.is_lift_down() and lift_down_wait_seconds > 0:
                self.manager.query_lift_state()
                await asyncio.sleep(1)
                lift_down_wait_seconds -= 1

            if lift_down_wait_seconds > 0:
                # Log the current door states
                open_count = self._count_and_print_door_states()

                self._log_message("Closing doors...")
                self.manager.door_open(opposites_day=True)

                self._log_message("Waiting 5 seconds...")
                await asyncio.sleep(5)

                # Log the current door states
                open_count = self._count_and_print_door_states()

            else:
                self._log_message("Not closing the doors as the lift did not get down")

            wait_guard_seconds = 0
            self.manager.query_plinky_plonky_state()
            # Wait for response
            await asyncio.sleep(1)
            while (not self.manager.is_plinky_plonky_at_reference()) and wait_guard_seconds < 60:
                self.manager.query_plinky_plonky_state()
                await asyncio.sleep(1)
                wait_guard_seconds += 1

            if wait_guard_seconds >= 60:
                 self._log_message("Plinky-plonky did not signal reference position"
                                   f" within {wait_guard_seconds} second(s), considering"
                                   f" the sequence completed anyway.")

            self.operation_status = "completed"
            self._log_message("Close sequence completed")

        except Exception as e:
            self.operation_status = "error"
            self._log_message(f"Close sequence error: {e}")
        finally:
            self._status_version += 1

    async def _check_doors_open(self):
        """
        Check if any door is open.
        Uses sensor (most reliable) or door state as fallback.
        """
        for ip, info in self.manager.devices.items():
            if self.manager.is_door(ip):
                # 1. First check sensor - if triggered, door is definitely open
                sensors = self.manager._door_sensors.get(ip, {})
                if sensors.get('open', False):
                    self._log_message(f"Door {ip} is OPEN (sensor triggered)")
                    return True
                
                # 2. If no sensor trigger, check door state
                state = self.manager.door_state(ip)
                if state == protocol.State.STATE_DOOR_STOPPED_OPEN:
                    self._log_message(f"Door {ip} is OPEN (state={state})")
                    return True
                elif state == protocol.State.STATE_DOOR_OPENING:
                    self._log_message(f"Door {ip} is OPENING")
                    # Still opening, not yet open - continue checking other doors
                    continue
                elif state == protocol.State.STATE_DOOR_STOPPED_CLOSED:
                    self._log_message(f"Door {ip} is CLOSED (state)")
                    continue
                else:
                    # State unknown, sensor not triggered - assume not open
                    self._log_message(f"Door {ip} state unknown, sensor not triggered - assuming closed")
        
        return False

    def _get_system_status(self):
        """Get current system status for API"""
        try:
            device_connected_ip_list = self.manager.get_device_ip_list(required=True, connected=True)
            device_initialised_ip_list = self.manager.get_device_ip_list(required=True, initialised=True)
            total = self.manager.device_count(required=True)
            
            # These now return single values (int) for the unique devices
            lift_state = self.manager.lift_state()
            stand_state = self.manager.stand_state()
            plinky_plonky_state = self.manager.plinky_plonky_state()
            
            # Doors still have multiple devices, so we need a dictionary
            door_state_dict = self.manager.door_state()  # This returns a dict of all doors
            
            return {
                'device_connected_ip_list': device_connected_ip_list,
                'device_initialised_ip_list': device_connected_ip_list,
                'total_devices': total,
                'auto_run': {
                    'enabled': self.auto_run_enabled,
                    'interval': self.auto_run_interval,
                    'sequence': self.auto_run_sequence,
                },
                'current_operation': {
                    'name': self.current_operation,
                    'status': self.operation_status,
                    'start_time': self.operation_start_time.isoformat() if self.operation_start_time else None
                },
                'lift_state': self._format_state_dict(lift_state),
                'door_state_dict': self._format_state_dict(door_state_dict),
                'stand_state': self._format_state_dict(stand_state),
                'plinky_plonky_state': self._format_state_dict(plinky_plonky_state)
            }
        except Exception as e:
            print(f"ERROR in _get_system_status: {e}")
            import traceback
            traceback.print_exc()
            raise

    def _format_state_dict(self, state_data):
        """Convert state values to readable names.
        Handles both dictionaries (multiple devices) and single values (single device).
        """
        if state_data is None:
            return {}
        
        # If it's a dictionary, format each entry
        if isinstance(state_data, dict):
            result = {}
            for ip, value in state_data.items():
                if value is not None:
                    try:
                        result[ip] = self.formatter.state(value, strip_prefix=True)
                    except:
                        result[ip] = f"0x{value:x}"
            return result
        
        # If it's a single value (int), format it
        if isinstance(state_data, int):
            try:
                return self.formatter.state(state_data, strip_prefix=True)
            except:
                return f"0x{state_data:x}"
        
        return state_data

    def _start_auto_run(self):
        """Start the auto-run task"""
        self.auto_run_task = asyncio.create_task(self._auto_run_loop())

    def _stop_auto_run(self):
        """Stop the auto-run task"""
        if self.auto_run_task:
            self.auto_run_task.cancel()
            self.auto_run_task = None

    def _restart_auto_run(self):
        """Restart the auto-run task with new settings"""
        self._stop_auto_run()
        self._start_auto_run()

    async def _auto_run_loop(self):
        """Background task for auto-run"""
        last_run = None        
        while self.auto_run_enabled:
            try:
                if self.operation_status == "idle" or self.operation_status == "completed":
                    if self.auto_run_sequence == "open_close":
                        # Store the start time before running
                        sequence_start = time.time()
                        
                        await self._run_open_sequence()
                        await asyncio.sleep(30)
                        await self._run_close_sequence()
                        
                        # Update last run time
                        last_run = sequence_start if last_run is None else sequence_start
                        
                        # Calculate remaining time to maintain fixed interval
                        elapsed = time.time() - sequence_start
                        remaining = max(0, self.auto_run_interval - elapsed)
                        
                        if remaining > 0:
                            try:
                                await asyncio.sleep(remaining)
                            except asyncio.CancelledError:
                                break
                    else:
                        # For other sequences, just use the interval
                        await asyncio.sleep(self.auto_run_interval)
                else:
                    # Not idle or completed, just wait and check again
                    await asyncio.sleep(1)
                    
            except asyncio.CancelledError:
                break
            except Exception as e:
                self._log_message(f"Auto-run error: {e}")
                await asyncio.sleep(self.auto_run_interval)
 
    def _get_html_template(self):
        """Return the HTML template as a string"""
        return '''<!DOCTYPE html>
<html>
<head>
    <title>Musical Box Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 1400px;
            margin: 0 auto;
            padding: 20px;
            background: #f5f5f5;
        }
        h1 { color: #333; margin-bottom: 5px; }
        .subtitle { color: #666; margin-top: 0; margin-bottom: 20px; }

        /* System Status Banner */
        .status-banner {
            background: #fff3e0;
            border-left: 4px solid #ffc107;
            padding: 12px 20px;
            margin-bottom: 20px;
            border-radius: 8px;
            display: flex;
            align-items: center;
            gap: 12px;
            font-weight: 500;
            transition: all 0.3s ease;
        }
        .status-banner.ready {
            background: #e8f5e9;
            border-left-color: #2e7d32;
        }
        .status-banner.waiting {
            background: #fff3e0;
            border-left-color: #ffc107;
        }
        .status-banner.error {
            background: #ffebee;
            border-left-color: #c62828;
        }
        .status-icon {
            font-size: 24px;
        }
        .status-text {
            flex: 1;
        }
        .status-details {
            font-size: 12px;
            color: #666;
            margin-top: 4px;
        }
        .status-banner.ready .status-details {
            color: #2e7d32;
        }

        /* Two-column layout for controls and settings */
        .dashboard {
            display: flex;
            gap: 20px;
            flex-wrap: wrap;
            margin-bottom: 20px;
        }
        .controls-column {
            flex: 2;
            min-width: 300px;
        }
        .settings-column {
            flex: 1;
            min-width: 280px;
        }

        .panel {
            background: white;
            border-radius: 8px;
            padding: 20px;
            margin-bottom: 20px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .panel h2 { margin-top: 0; color: #555; border-bottom: 1px solid #eee; padding-bottom: 10px; }
        .button-group {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
            margin: 15px 0;
        }
        button {
            padding: 10px 20px;
            font-size: 16px;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            transition: all 0.2s;
        }
        button:hover { opacity: 0.8; transform: translateY(-1px); }

        /* Sequence group - Green shades */
        .btn-sequence-primary { background: #2e7d32; color: white; }
        .btn-sequence-primary:hover { background: #1b5e20; }
        .btn-sequence-secondary { background: #4caf50; color: white; }
        .btn-sequence-secondary:hover { background: #388e3c; }

        /* Rotate group - Blue shades */
        .btn-rotate-primary { background: #0d47a1; color: white; }
        .btn-rotate-primary:hover { background: #0a3b7a; }
        .btn-rotate-secondary { background: #1976d2; color: white; }
        .btn-rotate-secondary:hover { background: #1565c0; }
        .btn-rotate-tertiary { background: #42a5f5; color: white; }
        .btn-rotate-tertiary:hover { background: #1e88e5; }

        /* Play group - Purple shades */
        .btn-play-primary { background: #4a148c; color: white; }
        .btn-play-primary:hover { background: #3c0c6e; }
        .btn-play-secondary { background: #7b1fa2; color: white; }
        .btn-play-secondary:hover { background: #6a1b9a; }

        /* Lift group - Teal shades */
        .btn-lift-primary { background: #00695c; color: white; }
        .btn-lift-primary:hover { background: #004d40; }
        .btn-lift-secondary { background: #009688; color: white; }
        .btn-lift-secondary:hover { background: #00796b; }

        /* Door group - Orange shades */
        .btn-door-primary { background: #e65100; color: white; }
        .btn-door-primary:hover { background: #bf360c; }
        .btn-door-secondary { background: #ff9800; color: white; }
        .btn-door-secondary:hover { background: #fb8c00; }

        /* Reboot group - Red shades */
        .btn-reboot-primary { background: #b71c1c; color: white; }
        .btn-reboot-primary:hover { background: #8b0000; }

        .status {
            padding: 10px;
            border-radius: 5px;
            margin: 10px 0;
            font-family: monospace;
        }
        .status.idle { background: #e8f5e9; color: #2e7d32; }
        .status.running { background: #fff3e0; color: #ef6c00; }
        .status.completed { background: #e8f5e9; color: #2e7d32; }
        .status.error { background: #ffebee; color: #c62828; }
        .device-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
            gap: 10px;
            margin: 10px 0;
        }
        .device-card {
            background: #f8f9fa;
            padding: 10px;
            border-radius: 5px;
            border-left: 3px solid #28a745;
        }
        .device-card.offline { border-left-color: #dc3545; }
        .device-card .name { font-weight: bold; }
        .device-card .ip { font-size: 12px; color: #666; font-family: monospace; }
        .device-card .state { font-size: 12px; margin-top: 5px; }
        input, select {
            padding: 8px;
            border: 1px solid #ddd;
            border-radius: 4px;
            font-size: 14px;
        }
        .settings-row {
            margin: 10px 0;
            display: flex;
            align-items: center;
            gap: 10px;
            flex-wrap: wrap;
        }
        .label { font-weight: bold; min-width: 100px; }
        .value { font-family: monospace; color: #007bff; }
        .debug-window {
            background: #1e1e1e;
            color: #d4d4d4;
            font-family: 'Courier New', monospace;
            font-size: 12px;
            padding: 10px;
            border-radius: 5px;
            height: 200px;
            overflow-y: auto;
            margin-top: 10px;
        }
        .debug-window .log-info { color: #4ec9b0; }
        .debug-window .log-warning { color: #dcdcaa; }
        .debug-window .log-error { color: #f48771; }
        .debug-window .log-debug { color: #9cdcfe; }
        .clear-logs {
            float: right;
            margin-bottom: 5px;
            padding: 5px 10px;
            font-size: 12px;
            background: #6c757d;
            color: white;
            border: none;
            border-radius: 3px;
            cursor: pointer;
        }
        .clear-logs:hover { background: #5a6268; }
        hr { margin: 20px 0; }
        .footer { text-align: center; color: #999; font-size: 12px; margin-top: 20px; clear: both; }

        /* Compact device grid for full-width panel */
        .compact-device-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
            gap: 8px;
            margin: 10px 0;
        }
        .compact-device-card {
            background: #f8f9fa;
            padding: 8px 12px;
            border-radius: 5px;
            border-left: 3px solid #28a745;
            font-size: 13px;
        }
        .compact-device-card.offline { border-left-color: #dc3545; }
        .compact-device-card .name { font-weight: bold; }
        .compact-device-card .ip { font-size: 11px; color: #666; font-family: monospace; }
        .compact-device-card .state { font-size: 11px; margin-top: 3px; }
    </style>
</head>
<body>
    <h1>🎵 Musical Box Controller</h1>
    <div class="subtitle">Remote control interface for automated musical box</div>

    <!-- System Status Banner -->
    <div id="systemStatusBanner" class="status-banner waiting">
        <div class="status-icon">⏳</div>
        <div class="status-text">
            <div>System Status: Initializing...</div>
            <div class="status-details">Waiting for devices to connect</div>
        </div>
    </div>

    <div class="dashboard">
        <div class="controls-column">
            <div class="panel">
                <h2>🎮 Controls</h2>

                <!-- Sequence group -->
                <div class="button-group">
                    <button class="btn-sequence-primary" onclick="sendCommand('open_sequence')">▶️ Open Sequence</button>
                    <button class="btn-sequence-secondary" onclick="sendCommand('close_sequence')">⏹️ Close Sequence</button>
                </div>

                <!-- Rotate group -->
                <div class="button-group">
                    <button class="btn-rotate-primary" onclick="sendCommand('stand_rotate', {direction: 'clockwise'})">🔄 Rotate Stand</button>
                    <button class="btn-rotate-secondary" onclick="sendCommand('stand_rotate', {direction: 'anticlockwise'})">🔄 Rotate Stand Backwards</button>
                    <button class="btn-rotate-tertiary" onclick="sendCommand('stand_stop')">⏹️ Stop Stand</button>
                </div>

                <!-- Play group -->
                <div class="button-group">
                    <button class="btn-play-primary" onclick="sendCommand('plinky_plonky_play')">🎵 Play Music</button>
                    <button class="btn-play-secondary" onclick="sendCommand('plinky_plonky_stop')">⏸️ Stop Music</button>
                </div>

                <!-- Lift group -->
                <div class="button-group">
                    <button class="btn-lift-primary" onclick="sendCommand('lift_up')">⬆️ Raise Lift</button>
                    <button class="btn-lift-secondary" onclick="sendCommand('lift_down')">⬇️ Lower Lift</button>
                </div>

                <!-- Door group -->
                <div class="button-group">
                    <button class="btn-door-primary" onclick="sendCommand('door_open', {index: -1})">🚪 Open All Doors</button>
                    <button class="btn-door-secondary" onclick="sendCommand('door_close', {index: -1})">🚪 Close All Doors</button>
                </div>

                <!-- Reboot group -->
                <div class="button-group">
                    <button class="btn-reboot-primary" onclick="sendCommand('reboot_all')">🔄 Reboot All</button>
                </div>
            </div>
        </div>

        <div class="settings-column">
            <div class="panel">
                <h2>⚙️ Auto-Run Settings</h2>
                <div class="settings-row">
                    <span class="label">Auto-run:</span>
                    <label><input type="radio" name="auto_enabled" value="true" onchange="setAutoRun(true)"> Enabled</label>
                    <label><input type="radio" name="auto_enabled" value="false" onchange="setAutoRun(false)" checked> Disabled</label>
                </div>
                <div class="settings-row">
                    <span class="label">Interval (seconds):</span>
                    <input type="number" id="interval" value="300" min="10" step="10" onblur="setInterval(this.value)">
                    <span>seconds</span>
                </div>
                <div class="settings-row">
                    <span class="label">Sequence:</span>
                    <select id="sequence" onchange="setSequence(this.value)">
                        <option value="open_close">Open → Wait → Close</option>
                        <option value="open_only">Open Only</option>
                        <option value="close_only">Close Only</option>
                    </select>
                </div>
                <div id="autoStatus" class="status idle" style="margin-top: 15px;">Auto-run: Disabled</div>
            </div>
        </div>
    </div>

    <!-- Full-width System Status Panel -->
    <div class="panel">
        <h2>📊 System Status</h2>
        <div id="operationStatus" class="status idle">Operation: None - idle</div>
        <div id="systemStatusCompact">Loading devices...</div>
    </div>

    <div class="panel">
        <h2>🐛 Debug Output
            <div style="float: right; display: flex; gap: 8px;">
                <button class="clear-logs" onclick="selectAllLogs()" style="background: #17a2b8;">📋 Select All</button>
                <button class="clear-logs" onclick="copyLogsToClipboard(event)" style="background: #28a745;">📋 Copy</button>
                <button class="clear-logs" onclick="clearLogs()">🗑️ Clear</button>
            </div>
        </h2>
        <div id="debugWindow" class="debug-window">Waiting for logs...</div>
    </div>

    <div class="footer">
        Musical Box Controller
    </div>

    <script>
        let statusSource = null;
        let logsSource = null;

        // Variables to track auto-scroll
        let autoScrollEnabled = true;
        let debugWindowScrollTimeout = null;

        function setupDebugWindow() {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            
            // Detect manual scroll
            debugWindow.addEventListener('scroll', function() {
                // Check if scrolled to bottom
                const isAtBottom = debugWindow.scrollHeight - debugWindow.scrollTop - debugWindow.clientHeight < 10;
                
                if (isAtBottom) {
                    autoScrollEnabled = true;
                } else {
                    autoScrollEnabled = false;
                    
                    // Auto-enable after 10 seconds of inactivity
                    if (debugWindowScrollTimeout) clearTimeout(debugWindowScrollTimeout);
                    debugWindowScrollTimeout = setTimeout(() => {
                        autoScrollEnabled = true;
                    }, 10000);
                }
            });
        }

        function selectAllLogs() {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            const range = document.createRange();
            range.selectNodeContents(debugWindow);
            const selection = window.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
        }

        function copyLogsToClipboard(event) {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            
            const text = debugWindow.innerText;
            
            // Try modern clipboard API first
            if (navigator.clipboard && navigator.clipboard.writeText) {
                navigator.clipboard.writeText(text).then(() => {
                    const btn = event.currentTarget;
                    const originalText = btn.textContent;
                    btn.textContent = '✓ Copied!';
                    setTimeout(() => {
                        btn.textContent = originalText;
                    }, 2000);
                }).catch(err => {
                    console.error('Failed to copy:', err);
                    fallbackCopy(text, event.currentTarget);
                });
            } else {
                // Fallback for older browsers
                fallbackCopy(text, event.currentTarget);
            }
        }

        function fallbackCopy(text, button) {
            const textarea = document.createElement('textarea');
            textarea.value = text;
            document.body.appendChild(textarea);
            textarea.select();
            try {
                document.execCommand('copy');
                const originalText = button.textContent;
                button.textContent = '✓ Copied!';
                setTimeout(() => {
                    button.textContent = originalText;
                }, 2000);
            } catch (err) {
                console.error('Fallback copy failed:', err);
                alert('Failed to copy logs. Please select and copy manually.');
            }
            document.body.removeChild(textarea);
        }

        async function sendCommand(cmd, params = {}) {
            try {
                const response = await fetch('/api/command', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({command: cmd, params: params})
                });
                const result = await response.json();

                // Command result will appear in the debug window via SSE
                // No need to add manually
                if (result.status === 'error') {
                    console.error(`[ERROR] ${result.message}`);
                } else {
                    console.log(`[INFO] ${result.message}`);
                }
            } catch (e) {
                console.error(`[ERROR] ${e.message}`);
            }
        }

        async function setAutoRun(enabled) {
            try {
                await fetch('/api/settings', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({auto_run_enabled: enabled})
                });
            } catch (e) {
                console.error(`Error setting auto-run: ${e.message}`);
            }
        }

        async function setInterval(interval) {
            try {
                await fetch('/api/settings', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({auto_run_interval: parseInt(interval)})
                });
            } catch (e) {
                console.error(`Error setting interval: ${e.message}`);
            }
        }

        async function setSequence(sequence) {
            try {
                await fetch('/api/settings', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({auto_run_sequence: sequence})
                });
            } catch (e) {
                console.error(`Error setting sequence: ${e.message}`);
            }
        }

        async function clearLogs() {
            try {
                await fetch('/api/logs/clear', {method: 'POST'});
                document.getElementById('debugWindow').innerHTML = 'Logs cleared...';
            } catch (e) {
                console.error('Error clearing logs:', e);
            }
        }

        function updateUI(status) {
            // Update auto-run UI
            const auto = status.auto_run;
            document.querySelector(`input[name="auto_enabled"][value="${auto.enabled}"]`).checked = auto.enabled;
            
            // Only update interval value if the field is NOT currently focused
            const intervalField = document.getElementById('interval');
            if (intervalField && document.activeElement !== intervalField) {
                intervalField.value = auto.interval;
            }
            
            document.getElementById('sequence').value = auto.sequence;
            
            const autoStatusDiv = document.getElementById('autoStatus');
            if (autoStatusDiv) {
                autoStatusDiv.className = `status ${auto.enabled ? 'running' : 'idle'}`;
                autoStatusDiv.innerHTML = auto.enabled ? 
                    `🔄 Auto-run: ${auto.sequence} every ${auto.interval}s` : 
                    `⏸️ Auto-run: Disabled`;
            }
            
            // Update operation status
            const op = status.current_operation;
            const opStatusDiv = document.getElementById('operationStatus');
            if (opStatusDiv) {
                opStatusDiv.className = `status ${op.status}`;
                opStatusDiv.innerHTML = `Operation: ${op.name || 'None'} - ${op.status}`;
            }
            
            // Update banner
            const totalDevices = status.total_devices;
            const connectedCount = status.device_connected_ip_list.length;
            const initialisedCount = status.device_initialised_ip_list.length;
            
            const banner = document.getElementById('systemStatusBanner');
            if (banner) {
                const bannerIcon = banner.querySelector('.status-icon');
                const bannerText = banner.querySelector('.status-text > div:first-child');
                const bannerDetails = banner.querySelector('.status-details');
                
                if (initialisedCount === totalDevices) {
                    banner.className = 'status-banner ready';
                    if (bannerIcon) bannerIcon.textContent = '✅';
                    if (bannerText) bannerText.textContent = 'System Status: READY TO PLAY!';
                    if (bannerDetails) bannerDetails.textContent = `All ${totalDevices} devices are connected and initialised.`;
                } else if (connectedCount === totalDevices) {
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '⏳';
                    if (bannerText) bannerText.textContent = 'System Status: Initializing...';
                    if (bannerDetails) bannerDetails.textContent = `${initialisedCount}/${totalDevices} devices ready, waiting for initialisation to complete.`;
                } else if (connectedCount > 0) {
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '🔌';
                    if (bannerText) bannerText.textContent = 'System Status: Connecting...';
                    if (bannerDetails) bannerDetails.textContent = `${connectedCount}/${totalDevices} devices connected. Waiting for more devices...`;
                } else {
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '⚠️';
                    if (bannerText) bannerText.textContent = 'System Status: Waiting for devices';
                    if (bannerDetails) bannerDetails.textContent = 'No devices connected yet. Check network and power.';
                }
            }
            
            // Update compact device grid
            let html = '<div class="compact-device-grid">';
            
            const uniqueDeviceIps = ['10.10.3.10', '10.10.3.20', '10.10.3.30'];
            for (let ip of uniqueDeviceIps) {
                if (status.device_connected_ip_list.includes(ip)) {
                    const initDone = status.device_initialised_ip_list.includes(ip);
                    let deviceName = '';
                    let stateValue = '';
                    
                    if (ip === '10.10.3.10') {
                        deviceName = 'Stand';
                        stateValue = status.stand_state;
                    } else if (ip === '10.10.3.20') {
                        deviceName = 'Lift';
                        stateValue = status.lift_state;
                    } else if (ip === '10.10.3.30') {
                        deviceName = 'Plinky Plonky';
                        stateValue = status.plinky_plonky_state;
                    }
                    
                    html += `<div class="compact-device-card ${initDone ? '' : 'offline'}">
                                <div class="name">${deviceName}</div>
                                <div class="ip">${ip}</div>`;
                    if (stateValue) {
                        html += `<div class="state">State: ${stateValue}</div>`;
                    }
                    html += `</div>`;
                }
            }
            
            for (let ip of status.device_connected_ip_list) {
                if (ip.startsWith('10.10.3.4')) {
                    const initDone = status.device_initialised_ip_list.includes(ip);
                    let doorNum = ip.split('.').pop();
                    const doorState = status.door_state_dict[ip];
                    
                    html += `<div class="compact-device-card ${initDone ? '' : 'offline'}">
                                <div class="name">Door ${doorNum}</div>
                                <div class="ip">${ip}</div>`;
                    if (doorState) {
                        html += `<div class="state">State: ${doorState}</div>`;
                    }
                    html += `</div>`;
                }
            }
            html += '</div>';
            
            const systemStatus = document.getElementById('systemStatusCompact');
            if (systemStatus) systemStatus.innerHTML = html;
        }

        let logBuffer = [];

        function updateLogs(newLogs) {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            
            if (newLogs.length === 0) return;
            
            // Remember scroll state
            const wasAtBottom = debugWindow.scrollHeight - debugWindow.scrollTop - debugWindow.clientHeight < 10;
            
            // Append new logs
            newLogs.forEach(log => {
                let className = 'log-info';
                if (log.includes('ERROR')) className = 'log-error';
                else if (log.includes('WARNING')) className = 'log-warning';
                else if (log.includes('DEBUG')) className = 'log-debug';
                const logDiv = document.createElement('div');
                logDiv.className = className;
                logDiv.textContent = log;
                debugWindow.appendChild(logDiv);
            });
            
            // Update buffer
            logBuffer = logBuffer.concat(newLogs);
            
            // Auto-scroll only if user was at bottom
            if (wasAtBottom) {
                debugWindow.scrollTop = debugWindow.scrollHeight;
            }
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        function appendLogs(newLogs) {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            
            // If we receive an empty array, clear the display
            if (newLogs.length === 0) {
                debugWindow.innerHTML = 'Logs cleared...';
                logBuffer = [];
                return;
            }
            
            // Check if user was at bottom
            const wasAtBottom = debugWindow.scrollHeight - debugWindow.scrollTop - debugWindow.clientHeight < 10;
            
            // If this is the first load (logBuffer empty), replace content
            if (logBuffer.length === 0) {
                debugWindow.innerHTML = newLogs.map(log => {
                    let className = 'log-info';
                    if (log.includes('ERROR')) className = 'log-error';
                    else if (log.includes('WARNING')) className = 'log-warning';
                    else if (log.includes('DEBUG')) className = 'log-debug';
                    return `<div class="${className}">${escapeHtml(log)}</div>`;
                }).join('');
            } else {
                // Append each new log
                newLogs.forEach(log => {
                    let className = 'log-info';
                    if (log.includes('ERROR')) className = 'log-error';
                    else if (log.includes('WARNING')) className = 'log-warning';
                    else if (log.includes('DEBUG')) className = 'log-debug';
                    const logDiv = document.createElement('div');
                    logDiv.className = className;
                    logDiv.textContent = log;
                    debugWindow.appendChild(logDiv);
                });
            }
            
            // Update buffer
            logBuffer = logBuffer.concat(newLogs);
            
            // Auto-scroll only if user was at bottom
            if (wasAtBottom) {
                debugWindow.scrollTop = debugWindow.scrollHeight;
            }
        }

        // Initialize SSE connections
        function setupStatusStream() {
            if (!window.EventSource) return;
            
            const source = new EventSource('/api/status/stream');
            
            source.onmessage = function(event) {
                try {
                    const status = JSON.parse(event.data);
                    updateUI(status);
                } catch (e) {
                    console.error("Error parsing status:", e);
                }
            };
            
            source.onerror = function() {
                console.log('Status stream error, reconnecting in 5 seconds...');
                source.close();
                setTimeout(setupStatusStream, 5000);
            };
            
            statusSource = source;
        }

        function setupLogsStream() {
            if (!window.EventSource) return;
            
            const source = new EventSource('/api/logs/stream');
            
            source.onmessage = function(event) {
                try {
                    const newLogs = JSON.parse(event.data);
                    if (newLogs.length > 0) {
                        appendLogs(newLogs);
                    }
                } catch (e) {
                    console.error("Error parsing logs:", e);
                }
            };
            
            source.onerror = function() {
                console.log('Logs stream error, reconnecting in 5 seconds...');
                source.close();
                setTimeout(setupLogsStream, 5000);
            };
            
            logsSource = source;
        }

        // Start the streams
        // Add a small delay before starting SSE to ensure page is fully loaded
        setTimeout(() => {
            setupStatusStream();
            setupLogsStream();
        }, 1000);
        setupDebugWindow();
    </script>
</body>
</html>'''

def get_ip_address():
    """Get our IP address"""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip


def main(http_port, esp32_port):
    # Create manager
    manager = MusicalBoxManager()

    # Create web interface
    web_interface = WebControlInterface(manager, host='0.0.0.0', port=http_port)

    # Start the server (non-blocking)
    receiver_thread = manager.start(esp32_port)

    # Run the web interface in an asyncio event loop
    loop = asyncio.new_event_loop()

    def run_web():
        asyncio.set_event_loop(loop)
        loop.run_until_complete(web_interface.start())
        loop.run_forever()

    web_thread = threading.Thread(target=run_web, daemon=True)
    web_thread.start()

    try:
        # Wait for all devices to connect and initialise
        if manager.wait_for_all_devices():
            print("Querying the state of all devices...")
            manager.query_stand_state()
            manager.query_lift_state()
            manager.query_plinky_plonky_state()
            for index in range(6): # 0 to 5
                manager.query_door_state(ip=None, index=index)
 
            print("=== Ready to play ===")
            print(f"Web interface available at http://{get_ip_address()}:{http_port}")

            # Process incoming messages
            while True:
                try:
                    ip, msg = manager.incoming_queue.get_nowait()
                    manager._process_message(ip, msg)
                except queue.Empty:
                    time.sleep(0.1)
        else:
            print("Failed to connect and initialise all devices")

    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        manager.stop()
        loop.call_soon_threadsafe(loop.stop)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=("Web interface to control the musical box."),
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('-p', type=int, default=HTTP_PORT_DEFAULT, help=("the HTTP port, default "
                                                                         f"{HTTP_PORT_DEFAULT}."))
    parser.add_argument('-e', type=int, default=ESP32_PORT_DEFAULT, help=("the ESP32 control port, default "
                                                                         f"{ESP32_PORT_DEFAULT}."))
    args = parser.parse_args()

    main(args.p, args.e)
