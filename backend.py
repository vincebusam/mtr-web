#!/usr/bin/env python3
# backend.py - Python WebSocket server for MTR

import asyncio
import json
import re
import subprocess
from aiohttp import web
import aiohttp_cors

routes = web.RouteTableDef()

def is_valid_target(target):
    """Validate hostname or IP address"""
    hostname_pattern = r'^[a-zA-Z0-9][a-zA-Z0-9\-_.]{0,253}[a-zA-Z0-9]$'
    ip_pattern = r'^(\d{1,3}\.){3}\d{1,3}$'
    return bool(re.match(hostname_pattern, target) or re.match(ip_pattern, target))

async def run_mtr(target, ws):
    """Run mtr with --split option and stream results"""
    try:
        # Run mtr with split output for real-time updates
        cmd = ['mtr', '--split', '--report-cycles', '20', target]
        
        process = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )
        
        # Stream stdout line by line
        while True:
            line = await process.stdout.readline()
            if not line:
                break
                
            decoded = line.decode('utf-8').strip()
            if decoded:
                try:
                    await ws.send_json({
                        'type': 'data',
                        'line': decoded
                    })
                except ConnectionResetError:
                    process.terminate()
                    break
        
        # Wait for process to complete
        await process.wait()
        
        # Check for errors
        stderr = await process.stderr.read()
        if process.returncode != 0:
            await ws.send_json({
                'type': 'error',
                'message': f'MTR failed: {stderr.decode()}'
            })
        else:
            await ws.send_json({'type': 'complete'})
            
    except FileNotFoundError:
        await ws.send_json({
            'type': 'error',
            'message': 'MTR not found. Please install mtr on the server.'
        })
    except Exception as e:
        try:
            await ws.send_json({
                'type': 'error',
                'message': f'Error running MTR: {str(e)}'
            })
        except ConnectionResetError:
            pass

@routes.get('/ws')
async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    
    async for msg in ws:
        if msg.type == web.WSMsgType.TEXT:
            data = json.loads(msg.data)
            
            if data.get('action') == 'start':
                target = data.get('target', '').strip()
                
                if not target or not is_valid_target(target):
                    await ws.send_json({
                        'type': 'error',
                        'message': 'Invalid target hostname or IP address'
                    })
                    continue
                
                await ws.send_json({
                    'type': 'started',
                    'target': target
                })
                
                await run_mtr(target, ws)
                
        elif msg.type == web.WSMsgType.ERROR:
            print(f'WebSocket error: {ws.exception()}')
    
    return ws

@routes.get('/')
async def index(request):
    return web.FileResponse('./index.html')

async def init_app():
    app = web.Application()
    app.add_routes(routes)
    
    # Setup CORS
    cors = aiohttp_cors.setup(app, defaults={
        "*": aiohttp_cors.ResourceOptions(
            allow_credentials=True,
            expose_headers="*",
            allow_headers="*",
        )
    })
    
    for route in list(app.router.routes()):
        cors.add(route)
    
    return app

if __name__ == '__main__':
    print("MTR WebSocket server starting on http://localhost:8022")
    print("Make sure 'mtr' is installed on your system")
    web.run_app(init_app(), host='0.0.0.0', port=8022)
