"""The master link and the camera command channel against a scripted port."""

import asyncio
import threading
import time

import pytest

from fsk_timekeeper.master import CameraLink, MasterLink, SessionLost

IDENTITY = "I FSK-WL 8 v1.0.0 0123ABCD89EF4567 M 17 922900 7 250.00 16000 0 12 1"


class FakePort:
    """Answers like the master: I to ?ID, the camera D line to ?STATUS, A V OK to V commands
    unless `replies` scripts otherwise (None = no answer)."""

    def __init__(self):
        self.buffer = b""
        self.lock = threading.Condition()
        self.written = []
        self.replies = []
        self.closed = False

    @property
    def in_waiting(self):
        return len(self.buffer)

    def read(self, n):
        with self.lock:
            if not self.buffer:
                self.lock.wait(0.05)
            data, self.buffer = self.buffer[:n], self.buffer[n:]
            return data

    def emit(self, line):
        with self.lock:
            self.buffer += (line + "\n").encode()
            self.lock.notify_all()

    def write(self, data):
        for line in data.decode().splitlines():
            self.written.append(line)
            if line == "?ID":
                self.emit(IDENTITY)
            elif line == "?STATUS":
                self.emit("H 1 2 3 0")
                self.emit("D 89EF4567 OK 0 0 0 0 0.00 0.00 0 0 0 0 1 1 1 XTAL 0 0 0 0 0 0 0 0 0 42 11 0 0 0 0 1")
            elif line.startswith("V "):
                reply = self.replies.pop(0) if self.replies else "A V OK"
                if reply:
                    self.emit(reply)

    def close(self):
        self.closed = True


@pytest.fixture
async def link():
    port = FakePort()
    master = MasterLink(lambda: port)
    lines = []
    master.subscribe(lambda line: lines.append(line.text))
    task = asyncio.create_task(master.run())
    for _ in range(100):
        if master.identity:
            break
        await asyncio.sleep(0.01)
    yield master, CameraLink(master), port, lines
    task.cancel()


async def test_identity_and_relay(link):
    master, _, port, lines = link
    assert master.identity == (8, "0123ABCD89EF4567", "M") and master.camera_node == "89EF4567"
    port.emit("H 5 6 7 0")
    await asyncio.sleep(0.1)
    assert lines[-1] == "H 5 6 7 0" and IDENTITY in lines
    assert master.write("PING") and port.written[-1] == "PING"


async def test_session_records_and_sequence(link):
    _, camera, port, _ = link
    assert await camera.start(400) == "42"
    assert port.written[-2:] == ["V S 400", "?STATUS"]
    assert await camera.send("L", 1, 4000) == 1
    assert await camera.send("K", 4000) is None
    assert await camera.send("C", 5000, 23, 91) == 2
    assert [w for w in port.written if w.startswith("V ") and w != "V S 400"] == ["V L 1 1 4000", "V K 2 4000", "V C 3 5000 23 91"]


async def test_wait_full_and_lost_answers_resend_the_same_record(link):
    _, camera, port, _ = link
    await camera.start(400)
    port.replies = ["X camwait", "X camfull", None, "A V OK"]
    t0 = time.monotonic()
    assert await camera.send("C", 100, 7, 99) == 1
    assert time.monotonic() - t0 >= 0.5  # one lost answer waited out
    assert [w for w in port.written if w.startswith("V C")] == ["V C 1 100 7 99"] * 4


async def test_line_mismatch_ends_the_session(link):
    _, camera, port, _ = link
    await camera.start(400)
    port.replies = ["X camline"]
    with pytest.raises(SessionLost) as lost:
        await camera.send("K", 10)
    assert lost.value.reason == "camline"
    with pytest.raises(SessionLost):
        await camera.send("K", 10)
    port.replies = ["X camoff"]
    with pytest.raises(SessionLost):
        await camera.start(400)
