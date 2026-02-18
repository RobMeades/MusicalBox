import asyncio
import ssl
import logging
import os
import time
from aiohttp import web
from aiohttp.web import HTTPException
from aiohttp.client_exceptions import ClientConnectorError
# These are the correct exceptions for server-side:
from aiohttp.web_exceptions import HTTPClientError
from aiohttp.web_ws import WebSocketError

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class RobustFileSender:
    """Helper class to handle file sending with error recovery"""
    
    def __init__(self, request, response, filepath, file_size):
        self.request = request
        self.response = response
        self.filepath = filepath
        self.file_size = file_size
        self.bytes_sent = 0
        self.start_time = time.time()
        
    async def send_file(self):
        try:
            with open(self.filepath, 'rb') as f:
                # Send first chunk (64KB)
                first_chunk = f.read(65536)
                if first_chunk and not await self.safe_write(first_chunk):
                    return False
                
                # Send remaining chunks
                while True:
                    chunk = f.read(16384)
                    if not chunk:
                        break
                    
                    if not await self.safe_write(chunk):
                        return False
                    
                    # Check transfer speed occasionally
                    if self.bytes_sent % (1024 * 1024) < 16384:  # Every ~1MB
                        elapsed = time.time() - self.start_time
                        if elapsed > 0:
                            speed = self.bytes_sent / elapsed / 1024  # KB/s
                            logger.debug(f"Transfer to {self.request.remote}: "
                                       f"{self.bytes_sent}/{self.file_size} bytes "
                                       f"({speed:.1f} KB/s)")
                
                logger.info(f"Complete transfer to {self.request.remote}: "
                          f"{self.bytes_sent} bytes in {time.time()-self.start_time:.1f}s")
                return True
                
        except FileNotFoundError:
            logger.error(f"File disappeared: {self.filepath}")
            return False
        except PermissionError:
            logger.error(f"Permission denied: {self.filepath}")
            return False
        except Exception as e:
            logger.error(f"Unexpected error in file send: {e}")
            return False
    
    async def safe_write(self, chunk):
        """Write chunk with error handling"""
        try:
            await self.response.write(chunk)
            self.bytes_sent += len(chunk)
            return True
        except ConnectionResetError as e:
            logger.info(f"Client {self.request.remote} connection reset: {e}")
            return False
        except ConnectionAbortedError as e:
            logger.info(f"Client {self.request.remote} connection aborted: {e}")
            return False
        except BrokenPipeError as e:
            logger.info(f"Client {self.request.remote} broken pipe: {e}")
            return False
        except asyncio.CancelledError:
            logger.info(f"Transfer cancelled for {self.request.remote}")
            return False
        except Exception as e:
            logger.error(f"Error writing to {self.request.remote}: {e}")
            return False

async def handle_firmware(request):
    # Get the filename from the URL path
    filename = request.match_info.get('filename', '')
    
    if not filename:
        return web.Response(status=400, text="No filename specified")
    
    # Security check
    if '..' in filename or filename.startswith('/'):
        logger.warning(f"Blocked directory traversal attempt: {filename} from {request.remote}")
        return web.Response(status=403, text="Invalid filename")
    
    filepath = os.path.join('.', filename)
    
    if not os.path.exists(filepath) or not os.path.isfile(filepath):
        logger.info(f"File not found: {filepath} requested by {request.remote}")
        return web.Response(status=404, text=f"File {filename} not found")
    
    file_size = os.path.getsize(filepath)
    logger.info(f"Serving {filename} ({file_size} bytes) to {request.remote}")
    
    # Create streaming response
    response = web.StreamResponse()
    response.headers['Content-Type'] = 'application/octet-stream'
    response.headers['Content-Length'] = str(file_size)
    response.headers['Cache-Control'] = 'no-cache'
    
    try:
        await response.prepare(request)
    except ConnectionResetError:
        logger.info(f"Client {request.remote} disconnected during prepare")
        return web.Response(status=499)
    except Exception as e:
        logger.error(f"Error preparing response for {request.remote}: {e}")
        return web.Response(status=500)
    
    # Use robust sender
    sender = RobustFileSender(request, response, filepath, file_size)
    success = await sender.send_file()
    
    if not success and sender.bytes_sent == 0:
        # Nothing was sent, return error
        return web.Response(status=500, text="Transfer failed")
    
    # Return the response (even if partial, aiohttp handles it)
    return response

async def main():
    app = web.Application()
    app.router.add_get('/{filename:.*}', handle_firmware)
    
    # Create SSL context
    ssl_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ssl_context.load_cert_chain('ca_cert.pem', 'ca_key.pem')
    
    # Configure server with timeouts
    runner = web.AppRunner(
        app, 
        keepalive_timeout=75,
        shutdown_timeout=60
    )
    await runner.setup()
    
    site = web.TCPSite(
        runner, 
        '0.0.0.0', 
        8070, 
        ssl_context=ssl_context,
        reuse_address=True,
        reuse_port=True,
        backlog=128  # Connection queue size
    )
    await site.start()
    
    logger.info("Server running on https://localhost:8070")
    logger.info(f"Current directory: {os.getcwd()}")
    
    # List available .bin files
    bin_files = [f for f in os.listdir('.') if f.endswith('.bin')]
    if bin_files:
        logger.info("Available .bin files:")
        for f in bin_files:
            logger.info(f"  - {f} ({os.path.getsize(f)} bytes)")
    
    try:
        await asyncio.Event().wait()
    except KeyboardInterrupt:
        logger.info("Shutting down...")
        await runner.cleanup()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server stopped by user")
    except Exception as e:
        logger.error(f"Fatal error: {e}", exc_info=True)
