"""The console's API: status, settings, clips, CORS, the master bridge and the live view."""

import asyncio

import numpy as np
import pytest
from aiohttp import WSServerHandshakeError

from fsk_timekeeper import protocol
from fsk_timekeeper.api import BUSY_CLOSE, make_app
from fsk_timekeeper.clips import ClipStore
from fsk_timekeeper.config import CameraConfig, Settings

ORIGIN = "https://luftaquila.github.io"


class FakeMaster:
    def __init__(self):
        self.written = []
        self.subscribers = []
        self.connected = True
        self.identity = (8, "0123ABCD89EF4567", "M")

    def subscribe(self, callback):
        self.subscribers.append(callback)
        return lambda: self.subscribers.remove(callback)

    def write(self, line):
        self.written.append(line)
        return True

    def emit(self, text):
        for callback in list(self.subscribers):
            callback(protocol.Line(text))


class FakePipeline:
    def __init__(self):
        self.restarts = 0
        self.holds = []
        self.viewers = 0
        self.serves = True  # whether a hold gets a session
        self.last_error = None

        class Live:
            stats = {"focus": 3.5, "mean": 120, "max": 250, "saturation": 0.001}

            def encode(self):
                return b"RIFF....WEBP"

        self.live = Live()

    def info(self):
        return {"api": 2, "app": "0.0.0", "usbProto": 8, "camera": {}, "session": {"id": "42"}, "storage": {}}

    def request_restart(self):
        self.restarts += 1

    async def hold(self, on):
        self.holds.append(on)
        return "42" if on and self.serves else None

    def watch(self, on):
        self.viewers += 1 if on else -1


@pytest.fixture
async def api(aiohttp_client, tmp_path):
    settings = Settings(origins=(ORIGIN,))
    store = ClipStore(str(tmp_path / "clips"))
    store.write("42", "pending-5", 1, 1500, lambda a, b: np.zeros((b - a + 1, 32), np.uint8), {"line": 5})
    store.commit("42", "pending-5", 3)
    master, pipeline = FakeMaster(), FakePipeline()
    config = CameraConfig(str(tmp_path / "config.json"))
    client = await aiohttp_client(make_app(settings, master, pipeline, store, config))
    return client, master, pipeline


async def test_info_config_and_cors(api):
    client, master, pipeline = api
    res = await client.get("/api/info", headers={"Origin": ORIGIN})
    assert res.status == 200 and res.headers["Access-Control-Allow-Origin"] == ORIGIN
    body = await res.json()
    assert body["session"]["id"] == "42" and body["master"]["identity"][1] == "0123ABCD89EF4567"
    res = await client.get("/api/info", headers={"Origin": "https://evil.example"})
    assert "Access-Control-Allow-Origin" not in res.headers
    pre = await client.options("/api/config", headers={"Origin": ORIGIN, "Access-Control-Request-Method": "PUT",
                                                       "Access-Control-Request-Private-Network": "true"})
    assert pre.status == 204 and pre.headers["Access-Control-Allow-Private-Network"] == "true"
    assert (await client.options("/api/config", headers={"Origin": "https://evil.example"})).status == 403
    body = await (await client.get("/api/config")).json()
    assert body["values"]["lineRateHz"] == 40000
    assert body["limits"]["lineRateHz"] == {"default": 40000.0, "min": 1000.0, "max": 59259.0}
    assert body["limits"]["exposureUs"]["max"] == 590 / 27  # what 40 kHz leaves after the readout
    assert body["lineTiming"] == {"masterHz": 16000000, "periodMin": 270, "periodMax": 16000,
                                  "cameraHz": 27000000, "readoutTicks": 85}
    res = await client.put("/api/config", json={"lineRateHz": 50000})
    assert res.status == 200 and (await res.json())["values"]["lineRateHz"] == 50000 and pipeline.restarts == 1
    res = await client.put("/api/config", json={"lineRateHz": 99999})
    assert res.status == 400 and pipeline.restarts == 1


async def test_clips(api):
    client, _, _ = api
    meta = await (await client.get("/api/clips/42/3")).json()
    assert (meta["seq"], meta["line"], meta["tiles"]) == (3, 5, 2)
    tile = await client.get("/api/clips/42/3/tiles/1.png")
    assert tile.status == 200 and (await tile.read())[:4] == b"\x89PNG"
    assert (await client.get("/api/clips/42/3/preview.webp")).status == 200
    for path in ("/api/clips/42/4", "/api/clips/42/3/tiles/9.png", "/api/clips/4x/3", "/api/clips/42/3/tiles/meta.json",
                 "/api/clips/..%2F../3"):
        assert (await client.get(path)).status == 404, path


async def test_bridge_relays_lines_drops_key_commands_and_serves_one_console(api):
    client, master, _ = api
    ws = await client.ws_connect("/api/master", headers={"Origin": ORIGIN})
    await ws.send_str("?ID\nK 0123\n?KEY\nC 1 11 ABCDEF01")
    await asyncio.sleep(0.05)
    assert master.written == ["?ID", "C 1 11 ABCDEF01"]
    master.emit("H 1 2 3 0")
    assert (await ws.receive_str(timeout=1)) == "H 1 2 3 0"
    second = await client.ws_connect("/api/master", headers={"Origin": ORIGIN})
    msg = await second.receive(timeout=1)
    assert second.close_code == BUSY_CLOSE or msg.data == BUSY_CLOSE
    with pytest.raises(WSServerHandshakeError):
        await client.ws_connect("/api/master", headers={"Origin": "https://evil.example"})
    await ws.close()
    await asyncio.sleep(0.05)
    again = await client.ws_connect("/api/master", headers={"Origin": ORIGIN})
    master.emit("A V OK")
    assert (await again.receive_str(timeout=1)) == "A V OK"
    await again.close()


async def test_live_view(api):
    client, _, pipeline = api
    ws = await client.ws_connect("/api/live", headers={"Origin": ORIGIN})
    stats = await ws.receive_json(timeout=1)
    assert stats["focus"] == 3.5
    assert (await ws.receive_bytes(timeout=1)).startswith(b"RIFF")
    assert pipeline.viewers == 1  # an open live view holds the camera session
    await ws.close()
    for _ in range(50):
        if pipeline.viewers == 0:
            break
        await asyncio.sleep(0.02)
    assert pipeline.viewers == 0


async def test_a_run_holds_the_camera_session(api):
    client, _, pipeline = api
    pre = await client.options("/api/session", headers={"Origin": ORIGIN, "Access-Control-Request-Method": "PUT"})
    assert pre.status == 204 and "PUT" in pre.headers["Access-Control-Allow-Methods"]
    res = await client.put("/api/session", json={"run": True}, headers={"Origin": ORIGIN})
    assert res.status == 200 and await res.json() == {"run": True, "session": "42"}
    res = await client.put("/api/session", json={"run": False})
    assert res.status == 200 and await res.json() == {"run": False, "session": None}
    for body in ({"run": 1}, {}, [True], "true"):
        assert (await client.put("/api/session", json=body)).status == 400
    assert pipeline.holds == [True, False]
    pipeline.serves, pipeline.last_error = False, "camoff"
    res = await client.put("/api/session", json={"run": True})
    assert res.status == 503 and (await res.json())["error"] == "Camera session did not start: camoff"
