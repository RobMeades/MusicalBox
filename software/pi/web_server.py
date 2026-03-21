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
        self.auto_run_interval = 300  # seconds (5 minutes default)
        self.auto_run_task = None
        self.auto_run_sequence = "open_close"  # 'open_close', 'open_only', 'close_only'

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
        status = self._get_system_status()
        return web.json_response(status)

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
                if self._status_version > last_version:
                    last_version = self._status_version
                    status = self._get_system_status()
                    await response.write(f"data: {json.dumps(status)}\n\n".encode())
                await asyncio.sleep(0.5)
        except Exception:
            pass
        finally:
            self.sse_clients.discard(client_id)

        return response

    async def handle_logs_stream(self, request):
        """Server-Sent Events stream for log updates"""
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
                if self._log_version > last_version:
                    last_version = self._log_version
                    logs = list(self.log_entries)
                    await response.write(f"data: {json.dumps(logs)}\n\n".encode())
                await asyncio.sleep(0.5)
        except Exception:
            pass
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
                self.manager.plinky_plonky_play(opposites_day=False)
                result['message'] = 'Plinky-plonky playing'

            elif command == 'plinky_plonky_stop':
                self.manager.plinky_plonky_play(opposites_day=True)
                result['message'] = 'Plinky-plonky stopped'

            elif command == 'door_open':
                index = params.get('index', -1)
                self.manager.door_open(index=index, opposites_day=False)
                result['message'] = f'Door {index if index >= 0 else "all"} opening'

            elif command == 'door_close':
                index = params.get('index', -1)
                self.manager.door_open(index=index, opposites_day=True)
                result['message'] = f'Door {index if index >= 0 else "all"} closing'

            elif command == 'lift_up':
                # Add debugging to see what's happening
                self._log_message("Checking if doors are open...")

                # First, log current door states
                door_states = self.manager.get_door_state()
                self._log_message(f"Current door states: {door_states}")

                # Also log which devices are doors
                for ip, info in self.manager.devices.items():
                    if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                        self._log_message(f"Door device {ip}: init_done={info['init_done']}, state={self.manager.door_states.get(ip)}")

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

    async def _run_open_sequence(self):
        """Run the complete open sequence"""
        self.operation_status = "running"
        self.current_operation = "open_sequence"
        self.operation_start_time = datetime.now()
        self._status_version += 1
        
        try:
            # 1. Open all doors
            self._log_message("Opening doors...")
            self.manager.door_open(index=-1, opposites_day=False)
            
            # Wait for doors to complete movement
            self._log_message("Waiting 8 seconds for doors to open...")
            await asyncio.sleep(8)
            
            # CRITICAL: Query door states to get actual stepper position
            self._log_message("Querying door states...")
            for ip, info in self.manager.devices.items():
                if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                    self.manager.query_door_state(ip)
            
            # Also query sensors for confirmation
            self._log_message("Querying door sensors...")
            for ip, info in self.manager.devices.items():
                if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                    self.manager.query_door_sensor(ip)
            
            # Wait for responses
            await asyncio.sleep(2)
            
            # Log the current door states
            self._log_message("Current door states:")
            for ip, info in self.manager.devices.items():
                if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                    state = self.manager.door_states.get(ip)
                    sensor = self.manager.door_sensors.get(ip, {}).get('open', False)
                    self._log_message(f"  Door {ip}: state={state}, sensor={sensor}")
            
            # 2. Raise lift
            self._log_message("Raising lift...")
            self.manager.lift_up(opposites_day=False)
            await asyncio.sleep(8)
            
            # 3. Start plinky-plonky
            self._log_message("Starting music...")
            self.manager.plinky_plonky_play(opposites_day=False)
            
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
            # 1. Stop plinky-plonky
            self._log_message("Stopping music...")
            self.manager.plinky_plonky_play(opposites_day=True)
            await asyncio.sleep(2)

            # 2. Lower lift
            self._log_message("Lowering lift...")
            self.manager.lift_up(opposites_day=True)
            await asyncio.sleep(8)  # Wait for lift to lower

            # 3. Close all doors
            self._log_message("Closing doors...")
            self.manager.door_open(index=-1, opposites_day=True)
            await asyncio.sleep(5)  # Wait for doors to close

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
            if info["init"] == protocol.Cmd.CMD_DOOR_INIT:
                # 1. First check sensor - if triggered, door is definitely open
                sensor = self.manager.door_sensors.get(ip, {})
                if sensor.get('open', False):
                    self._log_message(f"Door {ip} is OPEN (sensor triggered)")
                    return True
                
                # 2. If no sensor trigger, check door state
                state = self.manager.door_states.get(ip)
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
        return {
            'connected_devices': self.manager.get_connected_devices(),
            'initialized_devices': [ip for ip, info in self.manager.devices.items() if info["init_done"]],
            'total_devices': len(self.manager.devices),  # Add this
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
            'lift_state': self._format_state_dict(self.manager.get_lift_state()),
            'door_states': self._format_state_dict(self.manager.get_door_state()),
            'stand_state': self._format_state_dict(self.manager.get_stand_state()),
            'plinky_plonky_state': self._format_state_dict(self.manager.get_plinky_plonky_state())
        }

    def _format_state_dict(self, state_dict):
        """Convert state values to readable names"""
        result = {}
        for ip, value in state_dict.items():
            if value is not None:
                try:
                    result[ip] = self.formatter.state(value, strip_prefix=True)
                except:
                    result[ip] = f"0x{value:x}"
        return result

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
        while self.auto_run_enabled:
            try:
                await asyncio.sleep(self.auto_run_interval)

                if self.operation_status == "idle":
                    if self.auto_run_sequence == "open_close":
                        await self._run_open_sequence()
                        await asyncio.sleep(5)
                        await self._run_close_sequence()
                    elif self.auto_run_sequence == "open_only":
                        await self._run_open_sequence()
                    elif self.auto_run_sequence == "close_only":
                        await self._run_close_sequence()

            except asyncio.CancelledError:
                break
            except Exception as e:
                self._log_message(f"Auto-run error: {e}")

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
                    <input type="number" id="interval" value="300" min="10" step="10" onchange="setInterval(this.value)">
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
                <button class="clear-logs" onclick="copyLogsToClipboard()" style="background: #28a745;">📋 Copy</button>
                <button class="clear-logs" onclick="clearLogs()">🗑️ Clear</button>
            </div>
        </h2>
        <div id="debugWindow" class="debug-window">Waiting for logs...</div>
    </div>

    <div class="footer">
        Musical Box Controller | Read-only system
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

        function copyLogsToClipboard() {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            const text = debugWindow.innerText;
            navigator.clipboard.writeText(text).then(() => {
                // Show temporary feedback
                const btn = event.target;
                const originalText = btn.textContent;
                btn.textContent = '✓ Copied!';
                setTimeout(() => {
                    btn.textContent = originalText;
                }, 2000);
            }).catch(err => {
                console.error('Failed to copy:', err);
            });
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
            document.getElementById('interval').value = auto.interval;
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
            const connectedCount = status.connected_devices.length;
            const initializedCount = status.initialized_devices.length;
            
            const banner = document.getElementById('systemStatusBanner');
            if (banner) {
                const bannerIcon = banner.querySelector('.status-icon');
                const bannerText = banner.querySelector('.status-text > div:first-child');
                const bannerDetails = banner.querySelector('.status-details');
                
                if (initializedCount === totalDevices) {
                    // All devices ready
                    banner.className = 'status-banner ready';
                    if (bannerIcon) bannerIcon.textContent = '✅';
                    if (bannerText) bannerText.textContent = 'System Status: READY TO PLAY!';
                    if (bannerDetails) bannerDetails.textContent = `All ${totalDevices} devices are connected and initialized.`;
                } else if (connectedCount === totalDevices) {
                    // All connected but some not initialized
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '⏳';
                    if (bannerText) bannerText.textContent = 'System Status: Initializing...';
                    if (bannerDetails) bannerDetails.textContent = `${initializedCount}/${totalDevices} devices ready, waiting for initialization to complete.`;
                } else if (connectedCount > 0) {
                    // Some devices connected
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '🔌';
                    if (bannerText) bannerText.textContent = 'System Status: Connecting...';
                    if (bannerDetails) bannerDetails.textContent = `${connectedCount}/${totalDevices} devices connected. Waiting for more devices...`;
                } else {
                    // No devices connected
                    banner.className = 'status-banner waiting';
                    if (bannerIcon) bannerIcon.textContent = '⚠️';
                    if (bannerText) bannerText.textContent = 'System Status: Waiting for devices';
                    if (bannerDetails) bannerDetails.textContent = 'No devices connected yet. Check network and power.';
                }
            }
            
            // Update compact device grid
            let html = '<div class="compact-device-grid">';
            for (let ip of status.connected_devices) {
                const initDone = status.initialized_devices.includes(ip);
                const deviceName = ip === '10.10.3.10' ? 'Stand' :
                                   ip === '10.10.3.20' ? 'Lift' :
                                   ip === '10.10.3.30' ? 'Plinky Plonky' :
                                   ip.startsWith('10.10.3.4') ? `Door ${ip.slice(-1)}` : ip;
                html += `<div class="compact-device-card ${initDone ? '' : 'offline'}">
                            <div class="name">${deviceName}</div>
                            <div class="ip">${ip}</div>`;
                if (status.lift_state[ip]) html += `<div class="state">Lift: ${status.lift_state[ip]}</div>`;
                if (status.door_states[ip]) html += `<div class="state">Door: ${status.door_states[ip]}</div>`;
                if (status.stand_state[ip]) html += `<div class="state">Stand: ${status.stand_state[ip]}</div>`;
                if (status.plinky_plonky_state[ip]) html += `<div class="state">Plinky: ${status.plinky_plonky_state[ip]}</div>`;
                html += `</div>`;
            }
            html += '</div>';
            
            const systemStatus = document.getElementById('systemStatusCompact');
            if (systemStatus) systemStatus.innerHTML = html;
        }

        function updateLogs(logs) {
            const debugWindow = document.getElementById('debugWindow');
            if (!debugWindow) return;
            
            if (logs.length === 0) {
                debugWindow.innerHTML = 'No logs yet...';
            } else {
                const wasAtBottom = autoScrollEnabled;
                
                debugWindow.innerHTML = logs.map(log => {
                    let className = 'log-info';
                    if (log.includes('ERROR')) className = 'log-error';
                    else if (log.includes('WARNING')) className = 'log-warning';
                    else if (log.includes('DEBUG')) className = 'log-debug';
                    return `<div class="${className}">${escapeHtml(log)}</div>`;
                }).join('');
                
                if (wasAtBottom) {
                    debugWindow.scrollTop = debugWindow.scrollHeight;
                }
            }
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        // Set up SSE for status updates
        if (!!window.EventSource) {
            // Status stream
            statusSource = new EventSource('/api/status/stream');
            statusSource.onmessage = function(event) {
                const status = JSON.parse(event.data);
                updateUI(status);
            };
            statusSource.onerror = function() {
                console.log('Status stream disconnected, reconnecting...');
                setTimeout(() => {
                    window.location.reload();
                }, 5000);
            };

            // Logs stream
            logsSource = new EventSource('/api/logs/stream');
            logsSource.onmessage = function(event) {
                const logs = JSON.parse(event.data);
                updateLogs(logs);
            };
            logsSource.onerror = function() {
                console.log('Logs stream disconnected, reconnecting...');
                setTimeout(() => {
                    window.location.reload();
                }, 5000);
            };
        }
        
        // Initialize debug window
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
        # Wait for all devices to connect and initialize
        if manager.wait_for_all_devices():
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
            print("Failed to connect and initialize all devices")

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