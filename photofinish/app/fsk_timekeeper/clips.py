"""Clips of crossings on disk: <root>/<session>/<seq>/ holds full-resolution PNG tiles (lines
along x, tile_lines lines each), a WebP preview of the whole clip and meta.json. A clip is
written under a pending name until the master gives its crossing a capture_seq. The oldest
sessions go first when the clips exceed the quota."""

import json
import math
import os
import shutil
import time
from concurrent.futures import ThreadPoolExecutor

import cv2
import numpy as np

TILE_LINES = 1024
TILE_WRITERS = 3  # tiles encoded at once (OpenCV lets go of the GIL); a core stays for the frames
PREVIEW_MAX_WIDTH = 2000
PREVIEW_HEIGHT = 256
PREVIEW_QUALITY = 80


def write_tile(directory, read, first, last, i):
    """Tile i of lines first..last, fetched on its own and turned to pixel rows x lines. Returns
    its lines narrowed to PREVIEW_HEIGHT pixels (for the preview) and its pixel count, or None
    when read() found them gone."""
    a = first + i * TILE_LINES
    lines = read(a, min(last, a + TILE_LINES - 1))
    if lines is None:
        return None
    cv2.imwrite(os.path.join(directory, f"{i}.png"), cv2.transpose(lines))
    return cv2.resize(lines, (PREVIEW_HEIGHT, len(lines)), interpolation=cv2.INTER_AREA), lines.shape[1]


class ClipStore:
    def __init__(self, root, quota_bytes=50 * 10**9):
        self.root = root
        self.quota = quota_bytes
        self.writers = ThreadPoolExecutor(TILE_WRITERS, "clip")
        os.makedirs(root, exist_ok=True)

    def path(self, session, seq):
        return os.path.join(self.root, str(session), str(seq))

    def write(self, session, name, first, last, read, meta):
        """Write lines first..last as clip `name` of session, fetched a tile at a time with
        read(a, b) (None once they are gone: no whole copy of a long clip in memory). Returns its
        directory, or None (and nothing on disk) when lines were gone."""
        directory = self.path(session, name)
        os.makedirs(directory, exist_ok=True)
        count = last - first + 1
        tiles = math.ceil(count / TILE_LINES)
        parts = list(self.writers.map(lambda i: write_tile(directory, read, first, last, i), range(tiles)))
        if any(p is None for p in parts):
            shutil.rmtree(directory, ignore_errors=True)
            return None
        per = max(1, math.ceil(count / PREVIEW_MAX_WIDTH))
        width = max(1, math.ceil(count / per))
        narrowed = np.concatenate([p[0] for p in parts])  # lines x PREVIEW_HEIGHT
        preview = cv2.transpose(cv2.resize(narrowed, (PREVIEW_HEIGHT, width), interpolation=cv2.INTER_AREA))
        cv2.imwrite(os.path.join(directory, "preview.webp"), preview, [cv2.IMWRITE_WEBP_QUALITY, PREVIEW_QUALITY])
        full = {**meta, "session": str(session), "first": int(first), "last": int(last),
                "height": int(parts[0][1]), "tileLines": TILE_LINES, "tiles": tiles,
                "preview": {"width": width, "height": PREVIEW_HEIGHT, "linesPerPixel": per}, "created": time.time()}
        with open(os.path.join(directory, "meta.json"), "w") as f:
            json.dump(full, f)
        return directory

    def commit(self, session, name, seq, extra=None):
        """The pending clip becomes clip seq; extra (the reading) joins its meta."""
        src, dst = self.path(session, name), self.path(session, seq)
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.replace(src, dst)
        with open(os.path.join(dst, "meta.json")) as f:
            meta = json.load(f)
        meta["seq"] = int(seq)
        meta.update(extra or {})
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
