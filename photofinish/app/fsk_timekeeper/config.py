"""Settings: host-level ones from the environment, camera and detector ones in
<state>/config.json (edited from the console's camera setup, /api/config)."""

import json
import os
from dataclasses import dataclass, field

# Editable from the console: name -> (default, min, max), or (default, choices)
EDITABLE = {
    "lineRateHz": (40000.0, 1000.0, 58000.0),
    "exposureUs": (10.0, 1.0, 10000.0),
    "gainDb": (0.0, 0.0, 12.0),
    # The cars' direction as seen from the camera. A line-scan image puts the front first, so
    # cars moving right come out mirrored: clips and OCR flip them back.
    "travel": ("left", ("left", "right")),
    "bandTop": (0, 0, 4095),
    "bandBottom": (2048, 1, 4096),
    "threshold": (8.0, 0.5, 100.0),  # % of the band's windows that changed
    "clipBeforeMs": (200.0, 0.0, 2000.0),  # lines kept before the front and after the vehicle
    "clipMaxMs": (3000.0, 500.0, 10000.0),  # longest clip from the front (within ringSeconds)
    "clipQuotaGb": (5.0, 1.0, 2000.0),  # clips on disk; the oldest sessions go first
}
# File-only knobs: name -> default
FIXED = {
    "frameLines": 1000,  # lines per camera frame (buffer)
    "firstFrameId": 0,  # the camera's first frame id after acquisition starts; 0 = not known
    "strobeUs": 2.0,  # Strobe pulse, shorter than a line period
    "packetSize": 0,  # GevSCPSPacketSize, 0 = camera default
    "ringSeconds": 20.0,
    "enterMs": 2.0,
    "exitMs": 30.0,
    "initLines": 4000,
    "ocrTimeoutMs": 1500.0,
    "checkpointS": 1.0,
}


def env(name, default):
    return os.environ.get(name, default)


@dataclass
class Settings:
    serial: str = field(default_factory=lambda: env("FSK_TIMEKEEPER_SERIAL", "/dev/ttyUSB-fsk-timekeeper"))
    state: str = field(default_factory=lambda: env("FSK_TIMEKEEPER_STATE", "/var/lib/fsk-timekeeper"))
    bind: str = field(default_factory=lambda: env("FSK_TIMEKEEPER_BIND", "127.0.0.1"))
    port: int = field(default_factory=lambda: int(env("FSK_TIMEKEEPER_PORT", "8080")))
    origins: tuple = field(default_factory=lambda: tuple(o.strip() for o in env(
        "FSK_TIMEKEEPER_ORIGINS", "https://luftaquila.github.io").split(",") if o.strip()))
    camera: str = field(default_factory=lambda: env("FSK_TIMEKEEPER_DEVICE", ""))  # Aravis id, "" = first


class CameraConfig:
    def __init__(self, path):
        self.path = path
        self.values = {k: v[0] for k, v in EDITABLE.items()} | dict(FIXED)
        try:
            with open(path) as f:
                saved = json.load(f)
            self.values.update({k: saved[k] for k in self.values if k in saved})
        except (OSError, ValueError):
            pass

    def __getitem__(self, key):
        return self.values[key]

    def editable(self):
        """/api/config: the editable values with their default and range or choices."""
        limits = {}
        for key, spec in EDITABLE.items():
            limits[key] = {"default": spec[0], "choices": list(spec[1])} if len(spec) == 2 else \
                {"default": spec[0], "min": spec[1], "max": spec[2]}
        return {"values": {k: self.values[k] for k in EDITABLE}, "limits": limits}

    @property
    def clip_quota_bytes(self):
        return int(self.values["clipQuotaGb"] * 10**9)

    def update(self, patch):
        """Validate and apply an /api/config patch as a whole; returns the keys that changed."""
        values = dict(self.values)
        for key, value in patch.items():
            if key not in EDITABLE:
                raise ValueError(f"unknown setting {key}")
            if len(EDITABLE[key]) == 2:
                if value not in EDITABLE[key][1]:
                    raise ValueError(f"{key} must be one of {', '.join(EDITABLE[key][1])}")
                values[key] = value
                continue
            default, low, high = EDITABLE[key]
            try:
                number = float(value)
            except (TypeError, ValueError):
                raise ValueError(f"{key} must be a number") from None
            if not low <= number <= high:
                raise ValueError(f"{key} must be within {low}..{high}")
            values[key] = int(number) if isinstance(default, int) else number
        if values["bandTop"] >= values["bandBottom"]:
            raise ValueError("bandTop must be above bandBottom")
        # A camera that cannot expose a line within its period skips triggers, and the master ends
        # every session on a skipped trigger. (The camera's own overhead on top is a Phase 0 check.)
        period_us = 1e6 / values["lineRateHz"]
        if values["exposureUs"] >= period_us:
            raise ValueError(f"exposureUs must be shorter than the line period ({period_us:.1f} us)")
        changed = [k for k in EDITABLE if values[k] != self.values[k]]
        if changed:
            os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(values, f)
            os.replace(tmp, self.path)
            self.values = values
        return changed
