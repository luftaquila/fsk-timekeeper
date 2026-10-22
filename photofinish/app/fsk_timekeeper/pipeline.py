"""One camera session at a time: frames -> line numbers -> RAM ring -> vehicle fronts -> OCR and
clips -> V records to the master, in line order.

A session runs only while something holds it: a console's camera-mode run, from its Start (which
gets a session of its own, so the background is learnt then) until it stops, or an open live
view. With nothing holding it the master stops triggering and the camera waits.

Records leave in the order of their lines (the master refuses others): a crossing waits for its
OCR, and everything after it waits behind it. A vehicle's clip holds the whole vehicle with a
margin of its own length (in lines, so whatever its speed) before and after it: it is written
once the vehicle has left and its margin is in, or cut at its maximum length while the vehicle is
still in view, and takes its crossing's capture_seq once both are known (either may come first).
A checkpoint never passes a vehicle that
is still being confirmed, and a loss that cuts into one starts at its first line. Checkpoints
start once the detector has learnt the background: a vehicle crossing before that is not
reported. Any break in line numbering, a master
that drops the session, or a camera failure ends the session; a new one starts a second later
(the console sees a new camera session like a sensor reboot)."""

import asyncio
import collections
import logging
import math
import shutil
import time
from concurrent.futures import ThreadPoolExecutor

import cv2
import numpy as np

from . import __version__, protocol
from .detect import Detector
from .frames import Broken, FrameTracker
from .master import SessionLost
from .ring import LineRing

log = logging.getLogger(__name__)

QUEUE_FRAMES = 200
API_VERSION = 2
HOLD_TIMEOUT_S = 10.0  # a run's Start waits this long for its session


class Record:
    """A V record: C (crossing, ready once read), L or K."""

    def __init__(self, kind, *args):
        self.kind = kind
        self.args = args
        self.done = kind != "C"

    @property
    def line(self):
        return self.args[0]


class Crossing(Record):
    def __init__(self, front):
        super().__init__("C", front)
        self.front = front
        self.end = None  # the vehicle's last line, once it has left
        self.reading = False
        self.ocr = {"car": 0, "conf": 0, "text": ""}
        self.seq = None  # capture_seq, once sent
        self.clip = None  # pending clip name, once written
        self.committed = False

    def read(self, car, conf, text):
        self.args = (self.front, car, conf)
        self.ocr = {"car": car, "conf": conf, "text": text}
        self.done = True


class LiveView:
    """The last width columns at one column per `step` lines, rows downscaled to height."""

    def __init__(self, width=1024, height=256):
        self.width, self.height = width, height
        self.image = np.zeros((height, width), np.uint8)
        self.step = 40
        self.mirror = False  # cars moving right: newest lines on the left
        self.carry = 0
        self.stats = {}

    def feed(self, lines):
        picked = lines[self.carry::self.step]
        self.carry = (self.carry - len(lines)) % self.step
        if len(picked):
            cols = cv2.resize(np.ascontiguousarray(picked.T), (len(picked), self.height), interpolation=cv2.INTER_AREA)
            n = min(cols.shape[1], self.width)
            self.image = np.concatenate((self.image[:, n:], cols[:, -n:]), axis=1)
        last = lines[-1].astype(np.int16)
        self.stats = {"focus": float(np.abs(np.diff(last)).mean()), "mean": int(last.mean()), "max": int(last.max()),
                      "saturation": float((lines[-1] >= 250).mean())}

    def encode(self):
        ok, data = cv2.imencode(".webp", self.image[:, ::-1] if self.mirror else self.image, [cv2.IMWRITE_WEBP_QUALITY, 70])
        return data.tobytes() if ok else b""


class Stalled(Exception):
    """The session runs but the camera sends no frames."""


class Released(Exception):
    """Nothing holds the session any more."""


class Pipeline:
    def __init__(self, link, source, store, config, reader):
        self.link = link
        self.source = source
        self.store = store
        self.cfg = config
        self.reader = reader
        self.work = ThreadPoolExecutor(1, "frames")  # frames, ring and detector, in order
        self.side = ThreadPoolExecutor(1, "ocr")  # OCR, which a crossing waits for
        self.clipper = ThreadPoolExecutor(1, "clips")  # clip files, which nothing waits for
        self.session = None
        self.running = False
        self.starts = 0  # sessions started so far
        self.held = False  # by a console's camera-mode run
        self.viewers = 0  # open live views, which hold the session too
        self.wanted = asyncio.Event()  # set while held or viewed
        self.released = asyncio.Event()  # set while neither
        self.released.set()
        self.lost_lines = 0
        self.captured_lines = 0  # lines received whole, as lost_lines since the app started
        self.dropped = 0
        self.last_error = None
        self.last_frame = 0.0  # time.monotonic() of the latest frame
        self.stalled = False  # the camera to be reset before the next session
        self.period = protocol.period_for(config["lineRateHz"])
        self.loop = None
        self.restart = asyncio.Event()
        self._reset(self.period)

    # ---- per session state (owned by the frames thread while running) ----
    def _reset(self, period):
        cfg = self.cfg
        rate = protocol.line_rate(period)
        self.period = period
        self.tracker = FrameTracker(cfg["frameLines"], period, first_id=cfg["firstFrameId"] or None)
        width = self.source.info().get("width") or 2048
        self.ring = LineRing(int(rate * cfg["ringSeconds"]), width)
        self.detector = Detector(width, band=(int(cfg["bandTop"]), min(int(cfg["bandBottom"]), width)),
                                 threshold=cfg["threshold"] / 100.0, enter_lines=max(1, int(rate * cfg["enterMs"] / 1000)),
                                 exit_lines=max(1, int(rate * cfg["exitMs"] / 1000)), init_lines=int(cfg["initLines"]))
        self.live = LiveView()  # this session's lines only
        self.live.step = max(1, int(rate / 1000))
        self.mirror = cfg["travel"] == "right"
        self.live.mirror = self.mirror
        self.ocr_timeout_lines = int(rate * cfg["ocrTimeoutMs"] / 1000)
        self.clip_margin = cfg["clipMarginPct"] / 100  # of the vehicle's lines, before and after it
        self.clip_max = max(1, int(rate * cfg["clipMaxMs"] / 1000))  # lines of the vehicle at most
        self.processed = 0
        self.outbox = collections.deque()
        self.pending = {}  # front -> crossing not sent yet
        self.vehicles = {}  # front -> crossing whose clip is not written yet
        self.frames = asyncio.Queue(QUEUE_FRAMES)
        self.wake = asyncio.Event()
        self.last_checkpoint = 0.0

    def _on_frame(self, frame):
        """Camera thread."""
        self.last_frame = time.monotonic()
        self.loop.call_soon_threadsafe(self._enqueue, frame)

    def _enqueue(self, frame):
        try:
            self.frames.put_nowait(frame)
        except asyncio.QueueFull:
            self.dropped += 1  # its id gap reports the lines lost

    def _loss(self, first, last):
        """A loss from first, or from the first line of a vehicle being confirmed."""
        d = self.detector
        if not d.ready:
            return []  # the detector sees nothing before it has learnt the background
        if d.run and not d.in_vehicle and d.run_start is not None:
            first = min(first, d.run_start)
        d.gap()
        self.lost_lines += last - first + 1
        return [Record("L", first, last)]

    def _process(self, frame):
        """Frames thread: records and finished vehicles of one frame."""
        block = self.tracker.feed(frame)
        records, exits = [], []
        if block is None:
            return records, exits
        for first, last in block.lost:
            records += self._loss(first, last)
        height = self.tracker.height
        if block.lines is None:
            records += self._loss(block.first, block.first + height - 1)
            self.processed = block.first + height - 1
            return records, exits
        self.ring.write(block.first, block.lines)
        self.captured_lines += len(block.lines)
        self.live.feed(block.lines)
        for event in self.detector.process(block.first, block.lines):
            if event[0] == "enter":
                crossing = Crossing(event[1])
                self.pending[crossing.front] = crossing
                self.vehicles[crossing.front] = crossing
                records.append(crossing)
            elif event[0] == "exit" and not event[3]:
                crossing = self.vehicles.get(event[1]) or self.pending.get(event[1])  # clip or reading to come
                if crossing:
                    exits.append((crossing, event[2]))
        self.processed = block.first + len(block.lines) - 1
        return records, exits

    def _safe_line(self):
        """Lines every crossing through which has been queued."""
        d = self.detector
        if not d.ready:
            return 0
        if d.run and not d.in_vehicle and d.run_start is not None:
            return d.run_start - 1
        return self.processed

    # ---- loop side ----
    def _push(self, record):
        self.outbox.append(record)
        self.wake.set()

    def _finish(self, crossing, last):
        if crossing.reading:
            return
        crossing.reading = True
        self.loop.create_task(self._read(crossing, last))

    async def _read(self, crossing, last):
        """Read a vehicle's number from its lines front..last; then its crossing may go (unread if
        anything fails)."""
        loop = asyncio.get_running_loop()
        car, conf, text = 0, 0, ""
        try:
            lines = await loop.run_in_executor(self.work, self.ring.read, crossing.front, last)
            if lines is not None:
                car, conf, text = await loop.run_in_executor(self.side, self.reader.read, lines, self.mirror)
        except Exception:
            log.exception("reading the vehicle at line %s failed", crossing.front)
        crossing.read(car, conf, text)
        self.wake.set()

    def _clip_when_in(self, crossing, session):
        """Start a vehicle's clip once its lines are in: the vehicle and its margin, or, while it is
        still in view, its first clip_max lines (cut)."""
        if crossing.end is not None:
            margin = math.ceil(self.clip_margin * (crossing.end - crossing.front + 1))
            last, cut = crossing.end + margin, False
        elif self.processed - crossing.front + 1 >= self.clip_max:
            margin = math.ceil(self.clip_margin * self.clip_max)
            last, cut = crossing.front + self.clip_max - 1, True
        else:
            return
        if self.processed < last:
            return
        del self.vehicles[crossing.front]
        first = max(self.ring.oldest, crossing.front - margin)
        meta = {"line": crossing.front, "end": crossing.end, "cut": cut, "mirror": self.mirror}
        self.loop.create_task(self._clip(crossing, first, last, meta, self.ring, session))

    async def _clip(self, crossing, first, last, meta, ring, session):
        """Write a clip straight from its session's ring, a tile at a time; nothing waits for it."""
        loop = asyncio.get_running_loop()
        name = f"pending-{crossing.front}"
        try:
            written = await loop.run_in_executor(self.clipper, self.store.write, session, name, first, last,
                                                 ring.read_kept, meta)
        except Exception:
            log.exception("writing the clip of the vehicle at line %s failed", crossing.front)
            return
        if written is None:
            log.warning("the clip of the vehicle at line %s was overwritten before it was written", crossing.front)
            return
        crossing.clip = name
        await self._commit(crossing, session)

    async def _commit(self, crossing, session):
        """The pending clip becomes clip <capture_seq>, with the reading, once both are known."""
        if crossing.seq is None or crossing.clip is None or crossing.committed:
            return
        crossing.committed = True
        loop = asyncio.get_running_loop()
        try:
            await loop.run_in_executor(self.clipper, self.store.commit, session, crossing.clip, crossing.seq, {"ocr": crossing.ocr})
        except Exception:
            log.exception("committing the clip of the vehicle at line %s failed", crossing.front)

    async def _consume(self, session):
        loop = asyncio.get_running_loop()
        while True:
            frame = await self.frames.get()
            records, exits = await loop.run_in_executor(self.work, self._process, frame)
            for record in records:
                self._push(record)
            for crossing, end in exits:
                crossing.end = end
                self._finish(crossing, end)
            for crossing in list(self.pending.values()):
                if self.processed - crossing.front >= self.ocr_timeout_lines:
                    self._finish(crossing, self.processed)  # still in view: read what is there
            for crossing in list(self.vehicles.values()):
                self._clip_when_in(crossing, session)
            now = time.monotonic()
            if now - self.last_checkpoint >= self.cfg["checkpointS"] and self.detector.ready:
                self.last_checkpoint = now
                self._push(Record("K", self._safe_line()))

    async def _send(self, session):
        while True:
            while not self.outbox or not self.outbox[0].done:
                self.wake.clear()
                await self.wake.wait()
            record = self.outbox[0]
            if record.kind == "K" and len(self.outbox) > 1 and self.outbox[1].kind == "K":
                self.outbox.popleft()  # a newer checkpoint follows
                continue
            seq = await self.link.send(record.kind, *record.args)
            self.outbox.popleft()
            if record.kind == "C":
                self.pending.pop(record.front, None)
                record.seq = seq
                self.loop.create_task(self._commit(record, session))

    async def _session(self):
        loop = asyncio.get_running_loop()
        self.restart.clear()  # this session takes the current settings
        self.period = protocol.period_for(self.cfg["lineRateHz"])
        self._reset(self.period)
        errors = await loop.run_in_executor(self.side, self.source.configure, self.cfg.values, self.period)
        if errors:
            log.warning("camera settings not applied: %s", errors)
        self.source.start(self._on_frame)  # armed: lines come with the master's triggers
        try:
            session = await self.link.start(self.period)
            self.session = session
            self.running = True
            self.starts += 1
            self.last_error = None  # what ended an earlier session
            self.last_frame = time.monotonic()
            log.info("camera session %s at %.0f Hz", session, protocol.line_rate(self.period))
            await loop.run_in_executor(self.side, self.store.prune)
            async with asyncio.TaskGroup() as group:
                group.create_task(self._consume(session))
                group.create_task(self._send(session))
                group.create_task(self._wait_restart())
                group.create_task(self._watch_frames())
                group.create_task(self._wait_released())
        finally:
            self.running = False
            self.source.stop()

    async def _wait_restart(self):
        await self.restart.wait()
        self.restart.clear()
        raise SessionLost("restart")

    async def _wait_released(self):
        await self.released.wait()
        raise Released()

    async def _watch_frames(self):
        """A camera can keep exposing yet send nothing: frameTimeoutS without a frame ends the
        session, and the next one starts from a camera reset."""
        timeout = self.cfg["frameTimeoutS"]
        while True:
            await asyncio.sleep(timeout / 4)
            if time.monotonic() - self.last_frame > timeout:
                self.stalled = True
                raise Stalled(f"no frames for {timeout:g} s")

    async def _standby(self, retry_s):
        """Nothing holds the session: the master stops triggering (asked again until it answers),
        then the next session waits for a hold."""
        while not self.wanted.is_set():
            if await self.link.stop() == "ok":
                await self.wanted.wait()
                return
            try:
                await asyncio.wait_for(self.wanted.wait(), retry_s)
            except asyncio.TimeoutError:
                pass

    async def run(self, retry_s=1.0):
        self.loop = asyncio.get_running_loop()
        while True:
            if not self.wanted.is_set():
                await self._standby(retry_s)
            try:
                await self._session()
            except* Released:
                pass
            except* (SessionLost, Broken, Stalled) as group:
                self.last_error = str(group.exceptions[0])
                log.warning("camera session ended: %s", self.last_error)
            except* Exception as group:
                self.last_error = str(group.exceptions[0])
                log.exception("camera session failed", exc_info=group.exceptions[0])
            self.session = None
            if self.stalled:
                self.stalled = False
                log.warning("resetting the camera")
                await self.loop.run_in_executor(self.side, self.source.reset)
            if self.wanted.is_set():
                await asyncio.sleep(retry_s)

    def request_restart(self):
        """New settings: end the session; the next one takes them."""
        self.restart.set()

    async def hold(self, on, timeout=HOLD_TIMEOUT_S):
        """A console's camera-mode run holds the session from its Start (True) until it stops
        (False). A run not yet holding it gets a session started now, its background learnt at the
        Start (a live view's session gives way). The id of the session serving the hold once it
        runs, else None (released, or none in time)."""
        fresh = on and not self.held
        self.held = on
        if fresh and self.running:
            self.request_restart()
        self._holders_changed()
        if not on:
            return None
        if not fresh and self.running:
            return self.session
        started = self.starts
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while self.starts == started or not self.running:
            if not self.held or loop.time() > deadline:
                return None
            await asyncio.sleep(0.05)
        return self.session

    def watch(self, on):
        """A live view opens (True) or closes (False); an open one holds the session."""
        self.viewers += 1 if on else -1
        self._holders_changed()

    def _holders_changed(self):
        if self.held or self.viewers:
            self.released.clear()
            self.wanted.set()
        else:
            self.wanted.clear()
            self.released.set()

    def info(self):
        free = shutil.disk_usage(self.store.root).free
        lag = self.frames.qsize() * self.cfg["frameLines"] * self.period / 16_000
        return {
            "api": API_VERSION, "app": __version__, "usbProto": protocol.USB_PROTO,
            "camera": self.source.info(),
            "session": {"id": self.session, "running": self.running, "lineRateHz": round(protocol.line_rate(self.period)),
                        "lostLines": self.lost_lines, "capturedLines": self.captured_lines, "droppedFrames": self.dropped,
                        "lagMs": round(lag), "error": self.last_error},
            "storage": {"freeBytes": free},
        }
