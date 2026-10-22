"""Vehicle fronts on line-scan lines. The camera looks across the track at a fixed scene, mostly
asphalt, so an empty line repeats the background's texture; a vehicle in the measurement plane
replaces that texture over part of the detection band. Light changes (shadows, clouds, the sun)
keep the texture at another brightness, so brightness alone decides nothing.

  - background: per-pixel mean and noise over the first init_lines lines (track empty); the mean
    then follows empty lines slowly, the noise stays;
  - the band is cut into windows of `window` pixels (a remainder under one window stays out at
    the band's edges). A window changed when the line no longer correlates with the background's
    texture there (below min_corr, whatever the gain and offset). Where the background is flat
    (variance within texture_k times the noise), noise alone breaks the correlation, so the line
    must also show texture of its own there: a weak background texture, back with the light or
    above the noise anyway, still correlates and changes nothing;
  - a line shows a vehicle when changed windows cover threshold of the band;
  - enter_lines such lines in a row are a vehicle; its front is the first of them;
  - exit_lines lines without a vehicle end it.
After lost lines the detector cannot know when a vehicle already in view arrived: such a vehicle
is reported suppressed (its crossing lies in the loss)."""

import numpy as np


class Detector:
    def __init__(self, width, band=(0, None), threshold=0.08, enter_lines=80, exit_lines=1200,
                 init_lines=4000, window=16, texture_k=4.0, min_corr=0.5, alpha=0.0005):
        top, bottom = band[0], band[1] if band[1] is not None else width
        self.threshold = threshold
        self.enter_lines = int(enter_lines)
        self.exit_lines = int(exit_lines)
        self.init_lines = int(init_lines)
        self.texture_k = texture_k
        self.min_corr = min_corr
        self.alpha = alpha
        pixels = bottom - top
        self.size = max(1, min(int(window), pixels))  # pixels a window
        self.count = max(1, pixels // self.size)
        self.top = top + (pixels - self.count * self.size) // 2
        self.bottom = self.top + self.count * self.size
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
        """Sums over the windows along the last axis."""
        return values.reshape(values.shape[:-1] + (self.count, self.size)).sum(axis=-1)

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
        level = self._windows(self.mean) / self.size
        self.centred = self.mean - np.repeat(level, self.size)
        self.texture = self._windows(self.centred ** 2)  # sum of squares about the window mean
        self.level = np.maximum(level, 1.0)
        self.textured = self.texture / self.size > self.texture_k * self.noise_w

    def vehicle_lines(self, band):
        """Boolean per line: shows a vehicle (not a change of light)."""
        b = band.reshape(len(band), self.count, self.size)  # line, window, pixel
        sx = b.sum(axis=2)
        spread = np.maximum(np.einsum("lwp,lwp->lw", b, b) - sx * sx / self.size, 0.0)
        cross = np.einsum("lwp,wp->lw", b, self.centred.reshape(self.count, self.size))
        corr = cross / np.sqrt(np.maximum(spread * self.texture, 1e-6))
        brighter = np.maximum(1.0, sx / self.size / self.level)  # more light, more shot noise
        shows_texture = spread / self.size > self.texture_k * self.noise_w * brighter
        changed = (corr < self.min_corr) & (self.textured | shows_texture)
        return changed.sum(axis=1) >= self.threshold * self.count

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
            self.noise_w = self._windows(self.noise) / self.size
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
        empty = 0 if self.in_vehicle else len(band) - int(vehicle.sum())
        if empty:
            # Follow the light on empty lines only, one blended step per block.
            rate = 1.0 - (1.0 - self.alpha) ** empty
            seen = band if empty == len(band) else band[~vehicle]
            self.mean += rate * (seen.mean(axis=0) - self.mean)
            self._model()
        return events
