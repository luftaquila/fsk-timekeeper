"""RAM ring of camera lines by absolute line number (1.. per session)."""

import numpy as np


class LineRing:
    def __init__(self, capacity, width):
        self.capacity = int(capacity)
        self.width = int(width)
        self.data = np.zeros((self.capacity, self.width), dtype=np.uint8)
        self.newest = 0  # last line written; lines newest - capacity + 1 .. newest are kept

    def reset(self):
        self.newest = 0

    @property
    def oldest(self):
        return max(1, self.newest - self.capacity + 1)

    def write(self, first, lines):
        """lines[i] is line first + i; lines never go back. A gap leaves stale rows, never read
        because readers ask for lines the pipeline saw arrive."""
        n = len(lines)
        if n > self.capacity:
            lines = lines[-self.capacity:]
            first += n - self.capacity
            n = self.capacity
        start = (first - 1) % self.capacity
        end = start + n
        if end <= self.capacity:
            self.data[start:end] = lines
        else:
            split = self.capacity - start
            self.data[start:] = lines[:split]
            self.data[: end - self.capacity] = lines[split:]
        self.newest = max(self.newest, first + n - 1)

    def read(self, first, last):
        """Copy of lines first..last, or None when any of them is no longer (or not yet) kept."""
        if first > last or first < self.oldest or last > self.newest:
            return None
        start = (first - 1) % self.capacity
        n = last - first + 1
        end = start + n
        if end <= self.capacity:
            return self.data[start:end].copy()
        return np.concatenate((self.data[start:], self.data[: end - self.capacity]))
