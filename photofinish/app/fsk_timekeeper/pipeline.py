"""One camera session at a time: frames -> line numbers -> RAM ring -> vehicle fronts -> OCR and
clips -> V records to the master, in line order.

Records leave in the order of their lines (the master refuses others): a crossing waits for its
OCR and clip, and everything after it waits behind it. A checkpoint never passes a vehicle that
is still being confirmed, and a loss that cuts into one starts at its first line. Checkpoints
start once the detector has learnt the background: a vehicle crossing before that is not
reported. Any break in line numbering, a master
that drops the session, or a camera failure ends the session; a new one starts a second later
(the console sees a new camera session like a sensor reboot)."""

import asyncio
import collections
import logging
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
API_VERSION = 1


class Record:
    """A V record: C (crossing, ready once read), L or K."""

    def __init__(self, kind, *args):
        self.kind = kind
        self.args = args
        self.done = kind != "C"
        self.clip = None

    @property
    def line(self):
        return self.args[0]


class Crossing(Record):
    def __init__(self, front):
        super().__init__("C", front)
        self.front = front
        self.end = None
        self.reading = False
        self.ocr = {"car": 0, "conf": 0, "text": ""}

    def read(self, car, conf, text, clip):
        self.args = (self.front, car, conf)
        self.ocr = {"car": car, "conf": conf, "text": text}
        self.clip = clip
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


class Pipeline:
    def __init__(self, link, source, store, config, reader):
        self.link = link
        self.source = source
        self.store = store
        self.cfg = config
        self.reader = reader
        self.work = ThreadPoolExecutor(1, "frames")  # frames, ring and detector, in order
        self.side = ThreadPoolExecutor(1, "ocr")  # OCR and clip files
        self.live = LiveView()
        self.session = None
        self.running = False
        self.lost_lines = 0
        self.dropped = 0
        self.last_error = None
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
        self.live.step = max(1, int(rate / 1000))
        self.mirror = cfg["travel"] == "right"
        self.live.mirror = self.mirror
        self.ocr_timeout_lines = int(rate * cfg["ocrTimeoutMs"] / 1000)
        self.clip_before = int(rate * cfg["clipBeforeMs"] / 1000)
        self.clip_max = int(rate * cfg["clipMaxMs"] / 1000)
        self.processed = 0
        self.outbox = collections.deque()
        self.pending = {}
        self.frames = asyncio.Queue(QUEUE_FRAMES)
        self.wake = asyncio.Event()
        self.last_checkpoint = 0.0

    def _on_frame(self, frame):
        """Camera thread."""
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
        for first, last in block.lost:
            records += self._loss(first, last)
        height = self.tracker.height
        if block.lines is None:
            records += self._loss(block.first, block.first + height - 1)
            self.processed = block.first + height - 1
            return records, exits
        self.ring.write(block.first, block.lines)
        self.live.feed(block.lines)
        for event in self.detector.process(block.first, block.lines):
            if event[0] == "enter":
                crossing = Crossing(event[1])
                self.pending[crossing.front] = crossing
                records.append(crossing)
            elif event[0] == "exit" and not event[3] and event[1] in self.pending:
                exits.append((self.pending[event[1]], event[2]))
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

    def _clip_lines(self, front, end):
        """Frames thread: (ocr lines, clip first, clip lines) of a vehicle."""
        ocr = self.ring.read(front, end)
        first = max(self.ring.oldest, front - self.clip_before)
        last = min(end + self.clip_before, front + self.clip_max, self.ring.newest)
        return ocr, first, self.ring.read(first, last)

    # ---- loop side ----
    def _push(self, record):
        self.outbox.append(record)
        self.wake.set()

    def _finish(self, crossing, end, session):
        if crossing.reading:
            return
        crossing.reading = True
        crossing.end = end
        self.loop.create_task(self._read(crossing, end, session))

    async def _read(self, crossing, end, session):
        """Read and keep a vehicle's clip; then its crossing may go (unread if anything fails)."""
        loop = asyncio.get_running_loop()
        car, conf, text, name = 0, 0, "", None
        try:
            ocr, first, lines = await loop.run_in_executor(self.work, self._clip_lines, crossing.front, end)
            if ocr is not None:
                car, conf, text = await loop.run_in_executor(self.side, self.reader.read, ocr, self.mirror)
            if lines is not None:
                name = f"pending-{crossing.front}"
                meta = {"line": crossing.front, "end": end, "mirror": self.mirror, "ocr": {"car": car, "conf": conf, "text": text}}
                await loop.run_in_executor(self.side, self.store.write, session, name, first, lines, meta)
        except Exception:
            log.exception("reading the vehicle at line %s failed", crossing.front)
        crossing.read(car, conf, text, name)
        self.wake.set()

    async def _consume(self, session):
        loop = asyncio.get_running_loop()
        while True:
            frame = await self.frames.get()
            records, exits = await loop.run_in_executor(self.work, self._process, frame)
            for record in records:
                self._push(record)
            for crossing, end in exits:
                self._finish(crossing, end, session)
            for crossing in list(self.pending.values()):
                if self.processed - crossing.front >= self.ocr_timeout_lines:
                    self._finish(crossing, self.processed, session)  # still in view: read what is there
            now = time.monotonic()
            if now - self.last_checkpoint >= self.cfg["checkpointS"] and self.detector.ready:
                self.last_checkpoint = now
                self._push(Record("K", self._safe_line()))

    async def _send(self, session):
        loop = asyncio.get_running_loop()
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
                if record.clip:
                    await loop.run_in_executor(self.side, self.store.commit, session, record.clip, seq)

    async def _session(self):
        loop = asyncio.get_running_loop()
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
            log.info("camera session %s at %.0f Hz", session, protocol.line_rate(self.period))
            await loop.run_in_executor(self.side, self.store.prune)
            async with asyncio.TaskGroup() as group:
                group.create_task(self._consume(session))
                group.create_task(self._send(session))
                group.create_task(self._wait_restart())
        finally:
            self.running = False
            self.source.stop()

    async def _wait_restart(self):
        await self.restart.wait()
        self.restart.clear()
        raise SessionLost("restart")

    async def run(self, retry_s=1.0):
        self.loop = asyncio.get_running_loop()
        while True:
            try:
                await self._session()
            except* (SessionLost, Broken) as group:
                self.last_error = str(group.exceptions[0])
                log.warning("camera session ended: %s", self.last_error)
            except* Exception as group:
                self.last_error = str(group.exceptions[0])
                log.exception("camera session failed", exc_info=group.exceptions[0])
            self.session = None
            await asyncio.sleep(retry_s)

    def request_restart(self):
        """New settings: end the session; the next one takes them."""
        self.restart.set()

    def info(self):
        free = shutil.disk_usage(self.store.root).free
        lag = self.frames.qsize() * self.cfg["frameLines"] * self.period / 16_000
        return {
            "api": API_VERSION, "app": __version__, "usbProto": protocol.USB_PROTO,
            "camera": self.source.info(),
            "session": {"id": self.session, "running": self.running, "lineRateHz": round(protocol.line_rate(self.period)),
                        "lostLines": self.lost_lines, "droppedFrames": self.dropped, "lagMs": round(lag),
                        "error": self.last_error},
            "storage": {"freeBytes": free},
        }
