"""HTTP and WebSocket API for the console, on 127.0.0.1; `tailscale serve` publishes it as
https://<host> on the tailnet. CORS and WebSocket origins: the console's (FSK_TIMEKEEPER_ORIGINS).

    GET  /api/info                                 status (pipeline.info)
    GET  /api/config  PUT /api/config              camera settings (config.EDITABLE): { values, limits }
    PUT  /api/session                              { run }: a camera-mode run holds the camera session
                                                   (pipeline.hold); { run, session } once it runs
    GET  /api/clips/<session>/<seq>                clip metadata (clips.py)
    GET  /api/clips/<session>/<seq>/tiles/<i>.png  full-resolution tile
    GET  /api/clips/<session>/<seq>/preview.webp   downscaled clip
    WS   /api/master                               the master's lines, both ways; one console
    WS   /api/live                                 setup view: JSON stats, then a WebP image; holds
                                                   the camera session while open
The bridge never relays key commands (K, ?KEY): the fleet key stays off the network."""

import asyncio
import json
import logging
import re

from aiohttp import WSMsgType, web

from . import protocol

log = logging.getLogger(__name__)

BUSY_CLOSE = 4409
LIVE_PERIOD_S = 0.5
CLIENT_QUEUE = 20000
SESSION = re.compile(r"^\d{1,20}$")
SEQ = re.compile(r"^\d{1,10}$")
TILE = re.compile(r"^\d{1,5}\.png$")


def cors(origins):
    @web.middleware
    async def middleware(request, handler):
        origin = request.headers.get("Origin")
        allowed = origin in origins
        if request.method == "OPTIONS":
            response = web.Response(status=204 if allowed else 403)
        else:
            response = await handler(request)
        if allowed and not isinstance(response, web.WebSocketResponse):
            response.headers["Access-Control-Allow-Origin"] = origin
            response.headers["Vary"] = "Origin"
            response.headers["Access-Control-Allow-Methods"] = "GET, PUT, OPTIONS"
            response.headers["Access-Control-Allow-Headers"] = "Content-Type"
            if request.headers.get("Access-Control-Request-Private-Network") == "true":
                response.headers["Access-Control-Allow-Private-Network"] = "true"
        return response

    return middleware


def make_app(settings, master, pipeline, store, config):
    app = web.Application(middlewares=[cors(settings.origins)])
    state = {"client": None}

    def origin_ok(request):
        origin = request.headers.get("Origin")
        return origin is None or origin in settings.origins

    async def info(request):
        body = pipeline.info()
        body["master"] = {"connected": master.connected, "identity": list(master.identity) if master.identity else None}
        return web.json_response(body)

    async def get_config(request):
        return web.json_response(config.editable())

    async def put_config(request):
        try:
            changed = config.update(await request.json())
        except (ValueError, json.JSONDecodeError) as error:
            return web.json_response({"error": str(error)}, status=400)
        if changed:
            store.quota = config.clip_quota_bytes
            pipeline.request_restart()  # a new session takes the new line rate and settings
        return web.json_response(config.editable())

    async def put_session(request):
        try:
            run = (await request.json()).get("run")
        except (json.JSONDecodeError, AttributeError):
            run = None
        if not isinstance(run, bool):
            return web.json_response({"error": "run must be true or false"}, status=400)
        session = await pipeline.hold(run)
        if run and session is None:
            reason = f": {pipeline.last_error}" if pipeline.last_error else ""
            return web.json_response({"error": f"Camera session did not start{reason}"}, status=503)
        return web.json_response({"run": run, "session": session})

    def clip_args(request):
        session, seq = request.match_info["session"], request.match_info["seq"]
        if not SESSION.match(session) or not SEQ.match(seq):
            raise web.HTTPNotFound()
        return session, seq

    async def clip(request):
        meta = store.meta(*clip_args(request))
        if meta is None:
            raise web.HTTPNotFound()
        return web.json_response(meta)

    async def clip_file(request, name):
        path = store.file(*clip_args(request), name)
        if path is None:
            raise web.HTTPNotFound()
        return web.FileResponse(path, headers={"Cache-Control": "max-age=31536000, immutable"})

    async def tile(request):
        name = request.match_info["tile"]
        if not TILE.match(name):
            raise web.HTTPNotFound()
        return await clip_file(request, name)

    async def preview(request):
        return await clip_file(request, "preview.webp")

    async def master_ws(request):
        if not origin_ok(request):
            raise web.HTTPForbidden()
        ws = web.WebSocketResponse(heartbeat=10)
        await ws.prepare(request)
        if state["client"] is not None:
            await ws.close(code=BUSY_CLOSE, message=b"another console is connected")
            return ws
        state["client"] = ws
        queue = asyncio.Queue(CLIENT_QUEUE)

        def relay(line):
            try:
                queue.put_nowait(line.text)
            except asyncio.QueueFull:
                pass  # a stalled console: the master re-sends E lines until they are acked

        unsubscribe = master.subscribe(relay)

        async def pump():
            while True:
                await ws.send_str(await queue.get())

        sender = asyncio.create_task(pump())
        try:
            async for message in ws:
                if message.type != WSMsgType.TEXT:
                    continue
                for line in message.data.split("\n"):
                    line = line.strip()
                    if not line:
                        continue
                    if protocol.is_key_command(line):
                        log.warning("dropped a key command from the console")
                        continue
                    master.write(line)
        finally:
            unsubscribe()
            sender.cancel()
            state["client"] = None
        return ws

    async def live_ws(request):
        if not origin_ok(request):
            raise web.HTTPForbidden()
        ws = web.WebSocketResponse(heartbeat=10)
        await ws.prepare(request)

        async def drain():  # reading is what notices the client's close
            async for _ in ws:
                pass

        reader = asyncio.create_task(drain())
        pipeline.watch(True)
        try:
            while not reader.done():
                await ws.send_str(json.dumps(pipeline.live.stats))
                await ws.send_bytes(pipeline.live.encode())
                await asyncio.wait({reader}, timeout=LIVE_PERIOD_S)
        except ConnectionResetError:
            pass
        finally:
            reader.cancel()
            pipeline.watch(False)
        return ws

    app.router.add_get("/api/info", info)
    app.router.add_get("/api/config", get_config)
    app.router.add_put("/api/config", put_config)
    app.router.add_put("/api/session", put_session)
    app.router.add_get("/api/clips/{session}/{seq}", clip)
    app.router.add_get("/api/clips/{session}/{seq}/tiles/{tile}", tile)
    app.router.add_get("/api/clips/{session}/{seq}/preview.webp", preview)
    app.router.add_get("/api/master", master_ws)
    app.router.add_get("/api/live", live_ws)
    return app
