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
        self.stops = 0

    async def start(self, period):
        self.sessions.append(period)
        self.seq = self.last = 0
        return str(100 + len(self.sessions))

    async def stop(self):
        self.stops += 1
        return "ok"

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
    pipeline.watch(True)  # held throughout, as by an open live view
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
    await run_until(pipeline, lambda: pipeline.processed == 30000 and sum(r[0] == "C" for r in link.records) == 2
                    and store.meta("101", "1") is not None and store.meta("101", "2") is not None)
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
    assert info["api"] == 2 and info["usbProto"] == 8 and info["session"]["lineRateHz"] == 40000


class FakeReader:
    def read(self, lines, mirror=False):
        return 7, 99, "7"


class SlowStore(ClipStore):
    """Clips that take a while to write."""

    def write(self, *args):
        time.sleep(0.5)
        return super().write(*args)


async def wait_for(done, timeout=10):
    t0 = time.monotonic()
    while not done():
        assert time.monotonic() - t0 < timeout
        await asyncio.sleep(0.02)


async def test_a_clip_holds_the_vehicle_and_its_margins(tmp_path):
    cam = FastCamera(40, width=256, vehicles=[(6000, 2500, 7), (12000, 6000, 8)])
    link = FakeLink()
    store = ClipStore(str(tmp_path / "clips"))
    cfg = config(tmp_path)
    cfg.values["clipMaxMs"] = 100.0  # 4000 lines: the second vehicle is still in view then
    pipeline = Pipeline(link, cam, store, cfg, FakeReader())
    await run_until(pipeline, lambda: store.meta("101", "1") is not None and store.meta("101", "2") is not None)
    whole, cut = store.meta("101", "1"), store.meta("101", "2")
    # Half the vehicle's 2500 lines before and after it.
    assert (whole["line"], whole["end"], whole["first"], whole["last"], whole["cut"]) == (6000, 8499, 4750, 9749, False)
    assert whole["ocr"] == {"car": 7, "conf": 99, "text": "7"}
    # Still in view after 4000 lines: cut there, half of those before it.
    assert (cut["line"], cut["end"], cut["first"], cut["last"], cut["cut"]) == (12000, None, 10000, 15999, True)


async def test_a_crossing_goes_before_its_clip_is_written(tmp_path):
    cam = FastCamera(30, width=256, vehicles=[(6000, 2500, 7)])
    link = FakeLink()
    store = SlowStore(str(tmp_path / "clips"))
    pipeline = Pipeline(link, cam, store, config(tmp_path), FakeReader())
    await run_until(pipeline, lambda: any(r[0] == "C" for r in link.records))
    assert store.meta("101", "1") is None  # its clip is still being written
    await wait_for(lambda: store.meta("101", "1") is not None)
    assert store.meta("101", "1")["seq"] == 1


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
    assert pipeline.info()["session"]["capturedLines"] == 38 * LINES  # all but the skipped and the broken frame


async def test_numbering_break_starts_a_new_session(tmp_path, caplog):
    cam = FastCamera(30, width=256)
    cam.broken_at = 12
    link = FakeLink()
    pipeline = Pipeline(link, cam, ClipStore(str(tmp_path / "clips")), config(tmp_path), Reader())
    await run_until(pipeline, lambda: len(link.sessions) >= 2)
    assert link.sessions[:2] == [400, 400]
    assert "frames of time" in caplog.text  # what ended the first session


class FlakyLink(FakeLink):
    """A master that times out the first session start."""

    def __init__(self):
        super().__init__()
        self.attempts = 0

    async def start(self, period):
        self.attempts += 1
        if self.attempts == 1:
            raise SessionLost("timeout")
        return await super().start(period)


class StallingCamera(FastCamera):
    """Plays its frames, then sends nothing while still running; counts its resets."""

    def __init__(self, frames, **kw):
        super().__init__(frames, **kw)
        self.resets = 0

    def reset(self):
        self.resets += 1


async def test_a_session_without_frames_ends_and_resets_the_camera(tmp_path, caplog):
    cam = StallingCamera(5, width=256)
    link = FakeLink()
    cfg = config(tmp_path)
    cfg.values["frameTimeoutS"] = 0.3
    pipeline = Pipeline(link, cam, ClipStore(str(tmp_path / "clips")), cfg, FakeReader())
    await run_until(pipeline, lambda: cam.resets >= 1 and len(link.sessions) >= 2)
    assert "camera session ended: no frames for 0.3 s" in caplog.text


async def test_a_running_session_shows_no_earlier_error(tmp_path, caplog):
    link = FlakyLink()
    pipeline = Pipeline(link, FastCamera(30, width=256), ClipStore(str(tmp_path / "clips")), config(tmp_path), FakeReader())
    await run_until(pipeline, lambda: pipeline.running)
    assert "camera session ended: timeout" in caplog.text and link.attempts == 2
    assert pipeline.last_error is None


async def cancel(task):
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


async def test_a_session_runs_only_while_held(tmp_path):
    link = FakeLink()
    pipeline = Pipeline(link, FastCamera(2000, width=256), ClipStore(str(tmp_path / "clips")), config(tmp_path), FakeReader())
    task = asyncio.create_task(pipeline.run(retry_s=0.05))
    try:
        await wait_for(lambda: link.stops == 1)  # nothing holds it: the master stops triggering
        await asyncio.sleep(0.2)
        assert link.sessions == [] and not pipeline.running
        assert await pipeline.hold(True) == "101"  # a run's Start, once its session runs
        assert await pipeline.hold(True) == "101"  # said again: the same session
        assert await pipeline.hold(False) is None
        await wait_for(lambda: not pipeline.running and link.stops == 2)
        pipeline.watch(True)  # a live view
        await wait_for(lambda: pipeline.running)
        pipeline.watch(False)
        await wait_for(lambda: not pipeline.running and link.stops == 3)
        assert len(link.sessions) == 2 and pipeline.last_error is None
    finally:
        await cancel(task)


async def test_a_run_gets_a_session_started_at_its_start(tmp_path):
    link = FakeLink()
    pipeline = Pipeline(link, FastCamera(2000, width=256), ClipStore(str(tmp_path / "clips")), config(tmp_path), FakeReader())
    task = asyncio.create_task(pipeline.run(retry_s=0.05))
    try:
        pipeline.watch(True)  # a live view's session: its background was learnt before the Start
        await wait_for(lambda: pipeline.running)
        assert pipeline.session == "101"
        assert await pipeline.hold(True) == "102"
        assert await pipeline.hold(False) is None
        await asyncio.sleep(0.2)
        assert pipeline.running and pipeline.session == "102"  # the live view still holds it
        pipeline.watch(False)
        await wait_for(lambda: not pipeline.running)
    finally:
        await cancel(task)


class DeadLink(FakeLink):
    """A master that never takes a session."""

    async def start(self, period):
        raise SessionLost("timeout")


async def test_a_hold_without_a_session_in_time_gets_none(tmp_path):
    pipeline = Pipeline(DeadLink(), FastCamera(30, width=256), ClipStore(str(tmp_path / "clips")), config(tmp_path), FakeReader())
    task = asyncio.create_task(pipeline.run(retry_s=0.05))
    try:
        assert await pipeline.hold(True, timeout=0.3) is None
        assert pipeline.last_error == "timeout"
    finally:
        await cancel(task)
