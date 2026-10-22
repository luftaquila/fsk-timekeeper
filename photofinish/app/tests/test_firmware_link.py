"""The camera link against the firmware's own camera code (device/firmware/tests/camera_sim.c):
session start, records in order, waiting for anchors, skipped triggers, line mismatch."""

import asyncio
import os
import shutil
import subprocess
import threading
import time

import pytest

from fsk_timekeeper.master import CameraLink, MasterLink, SessionLost

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SIM = os.path.join(ROOT, "device", "firmware", "build", "host", "camera-sim")


def build():
    if shutil.which("make") is None or shutil.which("cc") is None:
        return False
    result = subprocess.run(["make", "-C", os.path.join(ROOT, "device", "firmware", "tests"), "../build/host/camera-sim"],
                            capture_output=True)
    return result.returncode == 0 and os.path.exists(SIM)


pytestmark = pytest.mark.skipif(not build(), reason="cannot build the firmware camera stand-in")


class ProcessPort:
    """A subprocess's stdin/stdout as the master's port."""

    def __init__(self, *args):
        self.proc = subprocess.Popen([SIM, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.lock = threading.Lock()

    @property
    def in_waiting(self):
        return 0

    def read(self, n):
        return os.read(self.proc.stdout.fileno(), 4096)

    def write(self, data):
        self.proc.stdin.write(data)

    @property
    def closed(self):
        return self.proc.poll() is not None

    def close(self):
        self.proc.kill()
        self.proc.wait()


async def open_link(*args):
    port = ProcessPort(*args)
    master = MasterLink(lambda: port)
    lines = []
    master.subscribe(lambda line: lines.append(line.text))
    task = asyncio.create_task(master.run())
    for _ in range(200):
        if master.identity:
            break
        await asyncio.sleep(0.01)
    return master, CameraLink(master), lines, task, port


def events(lines, kind):
    return [line.split(" ") for line in lines if line.startswith("E ") and line.split(" ")[3] == kind]


async def test_records_follow_the_strobes():
    master, camera, lines, task, port = await open_link()
    try:
        assert master.camera_node == "89EF4567"
        session = await camera.start(400)
        started = time.monotonic()
        assert session == "1001"
        await asyncio.sleep(0.2)  # 8000 lines at 40 kHz
        assert await camera.send("L", 1, 2000) == 1
        assert await camera.send("K", 3000) is None
        assert await camera.send("C", 5000, 23, 91) == 2
        await asyncio.sleep(0.05)
        v = events(lines, "V")[0]
        assert v[2] == "89EF4567" and v[7] == session and v[11] == "2" and v[12] == "5000"
        assert int(v[14]) == 400 and int(v[15]) < 5000 <= int(v[16]) and v[17:19] == ["23", "91"]
        loss = events(lines, "L")[0]
        assert loss[11] == loss[12] == "1" and int(loss[13]) < int(loss[14])
        k = events(lines, "K")[0]
        assert k[11] == "1" and int(loss[14]) <= int(k[12]) < int(v[13])
        # A line exposed after the last anchor (every 10 ms) waits for the next one, then goes through.
        exposed = int((time.monotonic() - started) * 40000) - 50
        assert await camera.send("C", exposed, 7, 99) == 3
        # A line the camera never exposed: the numbering is off, the session ends.
        with pytest.raises(SessionLost) as lost:
            await camera.send("K", 10**9)
        assert lost.value.reason == "camline"
    finally:
        task.cancel()
        port.close()


async def test_skipped_triggers_end_the_session():
    master, camera, lines, task, port = await open_link("3")  # three triggers without exposure at line 2000
    try:
        await camera.start(400)
        await asyncio.sleep(0.25)
        # The strobe count fell behind the triggers, as it would had the master missed strobes:
        # no line number can be trusted any more.
        with pytest.raises(SessionLost) as lost:
            await camera.send("C", 1000, 1, 99)
        assert lost.value.reason == "camline"
    finally:
        task.cancel()
        port.close()
