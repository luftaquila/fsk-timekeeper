"""Vehicle fronts on line-scan lines. The camera looks across the track at a fixed scene, mostly
asphalt, so an empty line repeats the background's texture; a vehicle in the measurement plane
replaces that texture over part of the detection band. Light changes (shadows, clouds, the sun)
keep the texture at another brightness, so brightness alone decides nothing.

  - background: per-pixel mean and noise over the first init_lines lines (track empty); the mean
    then follows empty lines slowly, the noise stays;
  - the band is cut into windows of about `window` pixels. A window where the background has
    texture (variance beyond texture_k times the noise) changed when the line no longer correlates
    with that texture (below min_corr, whatever the gain and offset); a flat window changed when
    the line shows texture there;
  - a line shows a vehicle when changed windows cover threshold of the band;
  - enter_lines such lines in a row are a vehicle; its front is the first of them;
  - exit_lines lines without a vehicle end it.
After lost lines the detector cannot know when a vehicle already in view arrived: such a vehicle
is reported suppressed (its crossing lies in the loss)."""

import numpy as np


class Detector:
    def __init__(self, width, band=(0, None), threshold=0.08, enter_lines=80, exit_lines=1200,
                 init_lines=4000, window=16, texture_k=4.0, min_corr=0.5, alpha=0.0005):
        self.top, self.bottom = band[0], band[1] if band[1] is not None else width
        self.threshold = threshold
        self.enter_lines = int(enter_lines)
        self.exit_lines = int(exit_lines)
        self.init_lines = int(init_lines)
        self.texture_k = texture_k
        self.min_corr = min_corr
        self.alpha = alpha
        pixels = self.bottom - self.top
        count = max(1, pixels // max(1, int(window)))
        self.starts = (np.arange(count) * pixels) // count
        self.sizes = np.diff(np.append(self.starts, pixels)).astype(np.float32)
        self.reset()

    def reset(self):
        self.mean = None
        self.m2 = None
        self.noise = None
        self.seen = 0
        self.ready = False
        self.in_vehicle = False
        self.suppressed = False
        self.front = None
        self.run = 0  # vehicle lines in a row (entering) or empty lines in a row (inside)
        self.run_start = None
        self.last_vehicle = None
        self.fresh = False

    def gap(self):
        """Lines were lost: a vehicle seen next may have arrived inside the gap."""
        self.fresh = True
        self.run = 0
        self.run_start = None

    def _windows(self, values):
        return np.add.reduceat(values, self.starts, axis=-1)

    def _learn(self, band):
        """Welford over the initial lines."""
        for row in band:
            self.seen += 1
            if self.mean is None:
                self.mean = row.copy()
                self.m2 = np.zeros_like(row)
                continue
            delta = row - self.mean
            self.mean += delta / self.seen
            self.m2 += delta * (row - self.mean)

    def _model(self):
        """Per-window background figures, after every change of the mean."""
        level = self._windows(self.mean) / self.sizes
        self.centred = self.mean - np.repeat(level, self.sizes.astype(int))
        self.texture = self._windows(self.centred ** 2)  # sum of squares about the window mean
        self.level = np.maximum(level, 1.0)
        self.textured = self.texture / self.sizes > self.texture_k * self.noise_w

    def vehicle_lines(self, band):
        """Boolean per line: shows a vehicle (not a change of light)."""
        sx = self._windows(band)
        spread = np.maximum(self._windows(band * band) - sx * sx / self.sizes, 0.0)
        corr = self._windows(band * self.centred) / np.sqrt(np.maximum(spread * self.texture, 1e-6))
        brighter = np.maximum(1.0, sx / self.sizes / self.level)  # more light, more shot noise
        flat_changed = spread / self.sizes > self.texture_k * self.noise_w * brighter
        changed = np.where(self.textured, corr < self.min_corr, flat_changed)
        return (changed * self.sizes).sum(axis=1) >= self.threshold * self.sizes.sum()

    def process(self, first, lines):
        """Events of lines first..: ('enter', front), ('exit', front, end, suppressed)."""
        band = lines[:, self.top:self.bottom].astype(np.float32)
        events = []
        if not self.ready:
            need = self.init_lines - self.seen
            self._learn(band[:need])
            if self.seen < self.init_lines:
                return events
            self.ready = True
            self.noise = np.maximum(self.m2 / max(1, self.seen - 1), 1.0)
            self.noise_w = self._windows(self.noise) / self.sizes
            self._model()
            band = band[need:]
            first += need
            if not len(band):
                return events
        vehicle = self.vehicle_lines(band)
        if self.fresh:
            self.fresh = False
            if vehicle[0] and not self.in_vehicle:
                self.in_vehicle = True
                self.suppressed = True
                self.front = first
                self.last_vehicle = first
                self.run = 0
        for i, v in enumerate(vehicle):
            line = first + i
            if self.in_vehicle:
                if v:
                    self.run = 0
                    self.last_vehicle = line
                    continue
                self.run += 1
                if self.run >= self.exit_lines:
                    events.append(("exit", self.front, self.last_vehicle, self.suppressed))
                    self.in_vehicle = False
                    self.suppressed = False
                    self.run = 0
                continue
            if not v:
                self.run = 0
                continue
            if self.run == 0:
                self.run_start = line
            self.run += 1
            if self.run >= self.enter_lines:
                self.in_vehicle = True
                self.front = self.run_start
                self.last_vehicle = line
                self.run = 0
                events.append(("enter", self.front))
        empty = band[~vehicle] if not self.in_vehicle else band[:0]
        if len(empty):
            # Follow the light on empty lines only, one blended step per block.
            rate = 1.0 - (1.0 - self.alpha) ** len(empty)
            self.mean += rate * (empty.mean(axis=0) - self.mean)
            self._model()
        return events
