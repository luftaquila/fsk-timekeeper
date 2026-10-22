"""Car numbers (one to three digits on 200 x 200 mm stickers) from a vehicle's lines.

A line-scan image stretches along x (lines) with the vehicle's speed. The sticker is square, so
the height in pixels over the width in lines of its bright region gives the stretch; the crop is
resampled to square before reading. Tesseract reads one line of digits; its word confidence is
the reading's. A reading counts only when it has as many digits as the sticker has dark marks
side by side: a digit Tesseract missed (say "2" of "23") reads as car 0 (unread) for the
operator, never as another car."""

import logging
import re
import subprocess

import cv2
import numpy as np

log = logging.getLogger(__name__)


def find_stickers(image, min_h=24, max_h=600, min_fill=0.6, marks=(0.01, 0.5)):
    """Bright, solid regions of image (pixel rows x lines) with dark marks inside (digits),
    largest first, as (x, y, w, h)."""
    blur = cv2.GaussianBlur(image, (5, 5), 0)
    level, bright = cv2.threshold(blur, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (9, 9))
    closed = cv2.morphologyEx(bright, cv2.MORPH_CLOSE, kernel)
    count, _, stats, _ = cv2.connectedComponentsWithStats(closed)
    boxes = []
    for x, y, w, h, area in stats[1:count]:
        if not (min_h <= h <= max_h and w >= 4 and area >= min_fill * w * h):
            continue
        inner = image[y + h // 8: y + h - h // 8, x + w // 8: x + w - w // 8]
        dark = float((inner < level).mean()) if inner.size else 0.0
        if marks[0] <= dark <= marks[1]:
            boxes.append((int(x), int(y), int(w), int(h)))
    return sorted(boxes, key=lambda b: b[2] * b[3], reverse=True)


def square(image, box, margin=0.08):
    """The box resampled to h x h (the sticker is square), with a margin inside its edge."""
    x, y, w, h = box
    mx, my = int(w * margin), int(h * margin)
    crop = image[y + my: y + h - my, x + mx: x + w - mx]
    side = max(8, h - 2 * my)
    return cv2.resize(crop, (side, side), interpolation=cv2.INTER_AREA if crop.shape[1] > side else cv2.INTER_CUBIC)


def tesseract_digits(img, binary="tesseract"):
    """(text, conf) of the most confident word, or ("", 0)."""
    ok, png = cv2.imencode(".png", img)
    if not ok:
        return "", 0
    try:
        out = subprocess.run([binary, "stdin", "stdout", "--psm", "7", "-c", "tessedit_char_whitelist=0123456789", "tsv"],
                             input=png.tobytes(), capture_output=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        log.warning("tesseract failed: %s", error)
        return "", 0
    best = ("", 0)
    for row in out.stdout.decode("utf-8", "replace").splitlines()[1:]:
        cols = row.split("\t")
        if len(cols) < 12 or not cols[11].strip():
            continue
        try:
            conf = float(cols[10])
        except ValueError:
            continue
        if conf > best[1]:
            best = (cols[11].strip(), int(round(conf)))
    return best


def digit_count(img, min_h=0.15):
    """Dark marks side by side on a squared sticker crop with dark digits: marks whose columns
    overlap are one digit (a broken stroke stays one), marks touching the crop's edge are not
    digits, and a digit is at least min_h of the side tall."""
    height, width = img.shape
    _, dark = cv2.threshold(img, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU)
    count, _, stats, _ = cv2.connectedComponentsWithStats(dark)
    marks = sorted((x, x + w, y, y + h) for x, y, w, h, area in stats[1:count]
                   if area >= 4 and x > 0 and y > 0 and x + w < width and y + h < height)
    digits = []
    for x0, x1, y0, y1 in marks:
        if digits and x0 < digits[-1][1]:
            d = digits[-1]
            digits[-1] = (d[0], max(d[1], x1), min(d[2], y0), max(d[3], y1))
        else:
            digits.append((x0, x1, y0, y1))
    return sum(1 for _, _, y0, y1 in digits if y1 - y0 >= min_h * height)


class Reader:
    def __init__(self, binary="tesseract", min_h=24, max_h=600):
        self.binary = binary
        self.min_h = min_h
        self.max_h = max_h

    def read(self, lines, mirror=False):
        """(car, conf, text) of a vehicle's lines (lines x pixels); mirror: flip the image (cars
        moving right as seen from the camera come out mirrored)."""
        image = np.ascontiguousarray(lines.T[:, ::-1] if mirror else lines.T)
        for box in find_stickers(image, self.min_h, self.max_h)[:3]:
            crop = square(image, box)
            for candidate in (crop, 255 - crop):  # dark digits on white, or the reverse
                text, conf = tesseract_digits(cv2.copyMakeBorder(candidate, 12, 12, 12, 12, cv2.BORDER_REPLICATE), self.binary)
                if re.fullmatch(r"\d{1,3}", text) and int(text) > 0 and len(text) == digit_count(candidate):
                    return int(text), conf, text
        return 0, 0, ""
