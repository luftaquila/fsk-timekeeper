"""Camera frames to session line numbers. Frame k of a session holds lines (k - 1) * H + 1 .. k * H,
H lines a frame, one line per master trigger; k counts frame ids from the first frame received
(cameras need not restart their ids with acquisition), or from first_id when the camera is known
to start there, which also catches a first frame lost whole. Integrity:
  - an incomplete frame or a frame id gap loses its lines; numbering holds (the ids count them);
  - frames further apart in camera time than their ids allow were dropped inside the camera
    without an id: every later line number would be off, so the session is broken.
GigE Vision 1.x frame ids are 16 bits and skip 0; larger ids mean the camera uses 64-bit ids. Id 0
names no frame: Aravis gives it to a frame whose leader was lost, which is skipped (the next id's
gap reports its lines)."""

from dataclasses import dataclass

import numpy as np

ID_CYCLE_16 = 65535  # ids 1..65535


@dataclass
class Frame:
    frame_id: int
    timestamp_ns: int  # camera clock; 0 when the camera gives none
    lines: np.ndarray | None  # (H, width) uint8, None when incomplete


@dataclass
class Block:
    first: int  # line number of lines[0]
    lines: np.ndarray | None  # None: lines first..first + H - 1 were lost
    lost: list  # (first, last) line ranges lost before this block (frame id gaps)


class FrameTracker:
    def __init__(self, height, period_ticks, first_id=None, tolerance=0.5):
        self.height = int(height)
        self.frame_ns = self.height * period_ticks * 1e9 / 16_000_000
        self.first_id = first_id
        self.tolerance = tolerance
        self.index = 0  # frame number of the last frame (1..)
        self.last_id = None
        self.last_ts = None
        self.wide = False  # 64-bit ids seen

    def _step(self, frame_id):
        if self.last_id is None:
            return 1 if self.first_id is None else max(1, frame_id - self.first_id + 1)
        if frame_id > ID_CYCLE_16:
            self.wide = True
        if self.wide:
            return frame_id - self.last_id
        return (frame_id - self.last_id) % ID_CYCLE_16

    def feed(self, frame):
        """Block of this frame, None for id 0; raises Broken when line numbers can no longer be
        trusted."""
        if frame.frame_id == 0:
            return None
        step = self._step(frame.frame_id)
        if step <= 0:
            raise Broken(f"frame id went back: {self.last_id} -> {frame.frame_id}")
        if self.last_ts and frame.timestamp_ns:
            elapsed = frame.timestamp_ns - self.last_ts
            frames = elapsed / self.frame_ns
            if abs(frames - step) > self.tolerance:
                raise Broken(f"frame {frame.frame_id}: {frames:.2f} frames of time for {step} ids")
        lost = []
        if step > 1:
            lost.append((self.index * self.height + 1, (self.index + step - 1) * self.height))
        self.index += step
        self.last_id = frame.frame_id
        if frame.timestamp_ns:
            self.last_ts = frame.timestamp_ns
        first = (self.index - 1) * self.height + 1
        return Block(first, frame.lines, lost)


class Broken(Exception):
    pass
