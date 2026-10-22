"""Clips of crossings on disk: <root>/<session>/<seq>/ holds full-resolution PNG tiles (lines
along x, tile_lines lines each), a WebP preview of the whole clip and meta.json. A clip is
written under a pending name until the master gives its crossing a capture_seq. The oldest
sessions go first when the clips exceed the quota."""

import json
import math
import os
import shutil
import time

import cv2
import numpy as np

TILE_LINES = 1024
PREVIEW_MAX_WIDTH = 2000
PREVIEW_HEIGHT = 256
PREVIEW_QUALITY = 80


class ClipStore:
    def __init__(self, root, quota_bytes=50 * 10**9):
        self.root = root
        self.quota = quota_bytes
        os.makedirs(root, exist_ok=True)

    def path(self, session, seq):
        return os.path.join(self.root, str(session), str(seq))

    def write(self, session, name, first, lines, meta):
        """Write lines (first.. ) as clip `name` of session; returns its directory."""
        directory = self.path(session, name)
        os.makedirs(directory, exist_ok=True)
        image = np.ascontiguousarray(lines.T)  # pixel rows x lines
        tiles = math.ceil(lines.shape[0] / TILE_LINES)
        for i in range(tiles):
            cv2.imwrite(os.path.join(directory, f"{i}.png"), image[:, i * TILE_LINES: (i + 1) * TILE_LINES])
        per = max(1, math.ceil(lines.shape[0] / PREVIEW_MAX_WIDTH))
        width = max(1, math.ceil(lines.shape[0] / per))
        preview = cv2.resize(image, (width, PREVIEW_HEIGHT), interpolation=cv2.INTER_AREA)
        cv2.imwrite(os.path.join(directory, "preview.webp"), preview, [cv2.IMWRITE_WEBP_QUALITY, PREVIEW_QUALITY])
        full = {**meta, "session": str(session), "first": int(first), "last": int(first + lines.shape[0] - 1),
                "height": int(lines.shape[1]), "tileLines": TILE_LINES, "tiles": tiles,
                "preview": {"width": width, "height": PREVIEW_HEIGHT, "linesPerPixel": per}, "created": time.time()}
        with open(os.path.join(directory, "meta.json"), "w") as f:
            json.dump(full, f)
        return directory

    def commit(self, session, name, seq):
        """The pending clip becomes clip seq."""
        src, dst = self.path(session, name), self.path(session, seq)
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.replace(src, dst)
        with open(os.path.join(dst, "meta.json")) as f:
            meta = json.load(f)
        meta["seq"] = int(seq)
        with open(os.path.join(dst, "meta.json"), "w") as f:
            json.dump(meta, f)

    def meta(self, session, seq):
        try:
            with open(os.path.join(self.path(session, seq), "meta.json")) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def file(self, session, seq, name):
        path = os.path.join(self.path(session, seq), name)
        return path if os.path.isfile(path) else None

    def usage(self):
        total = 0
        for base, _, files in os.walk(self.root):
            total += sum(os.path.getsize(os.path.join(base, f)) for f in files)
        return total

    def prune(self):
        """Drop the oldest sessions while over the quota; never the newest one."""
        sessions = sorted((e for e in os.scandir(self.root) if e.is_dir()), key=lambda e: e.stat().st_mtime)
        while len(sessions) > 1 and self.usage() > self.quota:
            shutil.rmtree(sessions.pop(0).path, ignore_errors=True)
