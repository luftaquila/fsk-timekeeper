"""Camera sources: the JHEL GigE Vision line-scan camera through Aravis, or a synthetic one for
development and tests. A source delivers frames (frames.Frame) to a callback from its own
thread once started; triggers come from the master, so frames flow only while a session runs.

GenICam feature names, entries and types are the camera's own (its XML, `arv-tool-0.8 genicam`);
features() lists them in the order they are applied. A name the camera lacks is reported, not
fatal."""

import logging
import threading
import time

import cv2
import numpy as np

from .frames import Frame

log = logging.getLogger(__name__)


def features(cfg, period_ticks):
    """(name, value) pairs for a session: line trigger from Line 0, Strobe on Line 1."""
    pairs = [
        ("AcquisitionMode", "Continuous"),
        ("PixelFormat", "Mono8"),
        ("Height", int(cfg["frameLines"])),
        ("TriggerSelector", "FrameBurstStart"),
        ("TriggerMode", "Off"),
        ("TriggerSelector", "LineStart"),
        ("TriggerMode", "On"),
        ("TriggerSource", "Line0"),
        ("TriggerActivation", "RisingEdge"),
        ("ExposureAuto", "Off"),
        ("ExposureTime", float(cfg["exposureUs"])),
        ("GainAuto", "Off"),
        ("Gain", float(cfg["gainDb"])),
        ("LineSelector", "Line1"),
        ("LineSource", "ExposureActive"),
        ("StrobeEnable", True),
        ("StrobeLineDuration", int(cfg["strobeUs"])),
        ("StrobeLineDelay", 0),
        ("FrameTimeoutEnable", True),
    ]
    if cfg["packetSize"]:
        pairs.append(("GevSCPSPacketSize", int(cfg["packetSize"])))
    return pairs


class AravisCamera:
    def __init__(self, device_id=""):
        import gi

        gi.require_version("Aravis", "0.8")
        from gi.repository import Aravis

        self.Aravis = Aravis
        self.device_id = device_id or None
        self.camera = None
        self.stream = None
        self.thread = None
        self.running = False
        self.errors = []
        self.width = 0

    def open(self):
        if self.camera is None:
            self.camera = self.Aravis.Camera.new(self.device_id)
            self.width = int(self.camera.get_region()[2])
        return self.camera

    def info(self):
        try:
            cam = self.open()
            return {"model": cam.get_model_name(), "serial": cam.get_device_serial_number(), "connected": True,
                    "width": self.width, "errors": self.errors}
        except Exception as error:  # no camera on the network
            self.camera = None
            return {"model": None, "serial": None, "connected": False, "errors": [str(error)]}

    def configure(self, cfg, period_ticks):
        device = self.open().get_device()
        self.errors = []
        for name, value in features(cfg, period_ticks):
            try:
                if isinstance(value, bool):
                    device.set_boolean_feature_value(name, value)
                elif isinstance(value, int):
                    device.set_integer_feature_value(name, value)
                elif isinstance(value, float):
                    device.set_float_feature_value(name, value)
                else:
                    device.set_string_feature_value(name, value)
            except Exception as error:
                self.errors.append(f"{name}: {error}")
                log.warning("camera feature %s=%s: %s", name, value, error)
        self.height = int(cfg["frameLines"])
        return self.errors

    def start(self, on_frame):
        cam = self.open()
        try:
            self.stream = cam.create_stream(None, None)
            payload = cam.get_payload()
            for _ in range(64):
                self.stream.push_buffer(self.Aravis.Buffer.new_allocate(payload))
            self.running = True
            cam.start_acquisition()
        except Exception:
            # A camera that still answers reads but no longer streams (its control lost): the
            # next session connects afresh, as info() does when a read fails.
            self.running = False
            self.stream = None
            self.camera = None
            raise
        self.thread = threading.Thread(target=self._pump, args=(on_frame,), daemon=True)
        self.thread.start()

    def _pump(self, on_frame):
        ok = self.Aravis.BufferStatus.SUCCESS
        while self.running:
            buffer = self.stream.timeout_pop_buffer(200_000)
            if buffer is None:
                continue
            lines = None
            if buffer.get_status() == ok:
                data = np.frombuffer(buffer.get_data(), dtype=np.uint8)
                lines = data[: self.height * self.width].reshape(self.height, self.width).copy()
            on_frame(Frame(int(buffer.get_frame_id()), int(buffer.get_timestamp()), lines))
            self.stream.push_buffer(buffer)

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
        if self.camera is not None:
            try:
                self.camera.stop_acquisition()
            except Exception as error:
                log.warning("stop acquisition: %s", error)
        self.stream = None
        self.thread = None

    def reset(self):
        """Restart the camera (DeviceReset): it comes back with its defaults, and the next
        session's open finds it again."""
        cam, self.camera, self.stream = self.camera, None, None
        if cam is not None:
            try:
                cam.get_device().execute_command("DeviceReset")
            except Exception as error:  # it may drop the link before it answers
                log.warning("camera reset: %s", error)


class SyntheticCamera:
    """A fixed scene with vehicles: (front line, length in lines, car number), each with a white
    square sticker carrying its number. frames() yields frames without a thread (tests);
    start() plays them at the line rate (development)."""

    def __init__(self, width=1024, vehicles=(), seed=1):
        rng = np.random.default_rng(seed)
        self.width = width
        self.background = (90 + 60 * np.sin(np.linspace(0, 9, width)) + rng.normal(0, 6, width)).clip(0, 255).astype(np.uint8)
        self.noise = rng
        self.vehicles = list(vehicles)
        self.height = 1000
        self.period = 400
        self.running = False
        self.thread = None
        self.errors = []
        self.skip_frames = set()  # frame ids lost in transit
        self.broken_frames = set()  # incomplete frames

    def info(self):
        return {"model": "Synthetic", "serial": "SIM", "connected": True, "width": self.width, "errors": []}

    def configure(self, cfg, period_ticks):
        self.height = int(cfg["frameLines"])
        self.period = period_ticks
        return []

    def sticker(self, number, lines):
        side = self.width // 5
        tile = np.full((side, side), 245, np.uint8)
        text = str(number)
        scale = side / 90
        size, _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, scale, max(2, int(scale * 2.5)))
        cv2.putText(tile, text, ((side - size[0]) // 2, (side + size[1]) // 2), cv2.FONT_HERSHEY_SIMPLEX,
                    scale, 10, max(2, int(scale * 2.5)), cv2.LINE_AA)
        return cv2.resize(tile, (lines, side), interpolation=cv2.INTER_AREA)  # stretched along lines

    def render(self, first, n):
        """Lines first..first + n - 1 (lines x pixels)."""
        lines = np.tile(self.background, (n, 1)).astype(np.int16)
        lines += self.noise.normal(0, 2, lines.shape).astype(np.int16)
        top, bottom = self.width // 5, self.width * 4 // 5
        for front, length, number in self.vehicles:
            lo, hi = max(front, first), min(front + length, first + n)
            if lo >= hi:
                continue
            lines[lo - first: hi - first, top:bottom] = 35
            s_lines = max(8, length // 3)
            s_front = front + length // 3
            s_lo, s_hi = max(s_front, first), min(s_front + s_lines, first + n)
            if s_lo < s_hi:
                patch = self.sticker(number, s_lines).T  # lines x pixels
                y = self.width * 2 // 5
                lines[s_lo - first: s_hi - first, y: y + patch.shape[1]] = patch[s_lo - s_front: s_hi - s_front]
        return lines.clip(0, 255).astype(np.uint8)

    def frames(self, count, first_id=1):
        for k in range(count):
            frame_id = first_id + k
            if frame_id in self.skip_frames:
                continue
            ts = int(k * self.height * self.period * 1e9 / 16_000_000) + 1
            lines = None if frame_id in self.broken_frames else self.render(k * self.height + 1, self.height)
            yield Frame(frame_id, ts, lines)

    def start(self, on_frame):
        self.running = True

        def play():
            frame_s = self.height * self.period / 16_000_000
            for frame in self.frames(10**9):
                if not self.running:
                    return
                on_frame(frame)
                time.sleep(frame_s)

        self.thread = threading.Thread(target=play, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
        self.thread = None

    def reset(self):
        """Nothing to restart."""
