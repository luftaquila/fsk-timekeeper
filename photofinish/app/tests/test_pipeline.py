"""Sessions end to end on the synthetic camera: records in line order, crossings with their
cars, clips under their capture_seq, losses for lost frames, a new session on a numbering break."""

import asyncio
import shutil
import threading
import time

import pytest

from fsk_timekeeper.camera import SyntheticCamera
from fsk_timekeeper.clips import ClipStore
from fsk_timekeeper.config import CameraConfig
from fsk_timekeeper.master import SessionLost
from fsk_timekeeper.ocr import Reader
from fsk_timekeeper.pipeline import Pipeline

LINES = 500  # per frame


class FakeLink:
    """The master's side of V records: q, capture_seq and the line order rules."""

    def __init__(self):
        self.sessions = []
        self.records = []
        self.seq = 0
        self.last = 0

    async def start(self, period):
        self.sessions.append(period)
        self.seq = self.last = 0
        return str(100 + len(self.sessions))

    async def stop(self):
        pass

    async def send(self, kind, *args):
        line = args[0]
        if line < self.last or (kind != "K" and line == self.last):
            raise SessionLost("camseq")
        self.last = args[1] if kind == "L" else line
        self.records.append((kind, *args))
        if kind == "K":
            return None
        self.seq += 1
        return self.seq


class FastCamera(SyntheticCamera):
    """Plays its frames quickly, a little slower than the pipeline takes them."""

    def __init__(self, frames, **kw):
        super().__init__(**kw)
        self.count = frames
        self.broken_at = None  # frame id from which time jumps a frame (a frame dropped in the camera)

    def start(self, on_frame):
        self.running = True

        def play():
            for frame in self.frames(self.count):
                if not self.running:
                    return
                if self.broken_at and frame.frame_id >= self.broken_at:
                    frame.timestamp_ns += int(self.height * self.period * 1e9 / 16e6)
                on_frame(frame)
                time.sleep(0.004)

        self.thread = threading.Thread(target=play, daemon=True)
        self.thread.start()


def config(tmp_path):
    cfg = CameraConfig(str(tmp_path / "config.json"))
    cfg.values.update({"frameLines": LINES, "initLines": 2000, "checkpointS": 0.05, "ocrTimeoutMs": 300.0})
    return cfg


async def run_until(pipeline, done, timeout=20, link=None):
    task = asyncio.create_task(pipeline.run(retry_s=0.05))
    t0 = time.monotonic()
    while not done():
        if time.monotonic() - t0 > timeout:
            task.cancel()
            raise AssertionError(f"pipeline did not finish: {pipeline.last_error} {getattr(pipeline.link, 'records', None)}")
        await asyncio.sleep(0.02)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


@pytest.mark.skipif(shutil.which("tesseract") is None, reason="tesseract not installed")
async def test_session_reports_crossings_in_line_order(tmp_path):
    cam = FastCamera(60, width=512, vehicles=[(6000, 2500, 7), (15000, 3000, 23)])
    link = FakeLink()
    store = ClipStore(str(tmp_path / "clips"))
    pipeline = Pipeline(link, cam, store, config(tmp_path), Reader())
    await run_until(pipeline, lambda: pipeline.processed == 30000 and sum(r[0] == "C" for r in link.records) == 2)
    kinds = [r[0] for r in link.records]
    crossings = [r for r in link.records if r[0] == "C"]
    assert crossings == [("C", 6000, 7, crossings[0][3]), ("C", 15000, 23, crossings[1][3])]
    assert all(c[3] > 50 for c in crossings)
    checkpoints = [r[1] for r in link.records if r[0] == "K"]
    assert checkpoints == sorted(checkpoints)
    assert "L" not in kinds  # the lines before the background was learnt are not reported
    # Clips under the crossings' capture_seq.
    for seq, front in ((1, 6000), (2, 15000)):
        meta = store.meta("101", str(seq))
        assert meta["seq"] == seq and meta["line"] == front and meta["first"] <= front <= meta["last"]
        assert meta["ocr"]["car"] in (7, 23) and meta["mirror"] is False
    info = pipeline.info()
    assert info["api"] == 1 and info["usbProto"] == 8 and info["session"]["lineRateHz"] == 40000


async def test_lost_frames_are_reported_and_cut_a_vehicle_being_confirmed(tmp_path):
    cam = FastCamera(40, width=256, vehicles=[(9990, 2000, 7)])
    cam.skip_frames = {21}  # lines 10001..10500, while the vehicle from 9990 is being confirmed
    cam.broken_frames = {30}  # lines 14501..15000 arrive incomplete
    link = FakeLink()
    pipeline = Pipeline(link, cam, ClipStore(str(tmp_path / "clips")), config(tmp_path), Reader())
    await run_until(pipeline, lambda: pipeline.processed == 20000 and sum(r[0] == "L" for r in link.records) == 2)
    losses = [r for r in link.records if r[0] == "L"]
    assert losses == [("L", 9990, 10500), ("L", 14501, 15000)]
    assert not [r for r in link.records if r[0] == "C"]  # its front lies in the loss
    assert pipeline.lost_lines == 511 + 500


async def test_numbering_break_starts_a_new_session(tmp_path):
    cam = FastCamera(30, width=256)
    cam.broken_at = 12
    link = FakeLink()
    pipeline = Pipeline(link, cam, ClipStore(str(tmp_path / "clips")), config(tmp_path), Reader())
    await run_until(pipeline, lambda: len(link.sessions) >= 2)
    assert link.sessions[:2] == [400, 400]
    assert "frames of time" in pipeline.last_error
