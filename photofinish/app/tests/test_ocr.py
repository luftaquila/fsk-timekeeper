"""Car numbers from synthetic vehicles: a white square sticker stretched by speed."""

import os
import shutil

import cv2
import numpy as np
import pytest

from fsk_timekeeper.camera import SyntheticCamera
from fsk_timekeeper.ocr import Reader, digit_count, find_stickers

pytestmark = pytest.mark.skipif(shutil.which("tesseract") is None, reason="tesseract not installed")


@pytest.mark.parametrize("number,length", [(7, 3000), (23, 1200), (100, 6000)])
def test_reads_the_sticker_at_any_speed(number, length):
    cam = SyntheticCamera(width=512, vehicles=[(1, length, number)])
    lines = cam.render(1, length)
    car, conf, text = Reader().read(lines)
    assert car == number and text == str(number)
    assert conf > 50


def test_a_car_moving_right_reads_once_flipped():
    cam = SyntheticCamera(width=512, vehicles=[(1, 2000, 23)])
    mirrored = cam.render(1, 2000)[::-1]  # the front first: the side comes out mirrored
    car, _, text = Reader().read(mirrored, mirror=True)
    assert (car, text) == (23, "23")


def test_no_sticker_reads_unread():
    cam = SyntheticCamera(width=512, vehicles=[])
    lines = cam.render(1, 2000)
    assert Reader().read(lines) == (0, 0, "")
    assert find_stickers(lines.T.copy()) == []


@pytest.mark.parametrize("text", ["7", "23", "100", "023"])
def test_digit_count_counts_the_marks_side_by_side(text):
    side = 120
    tile = np.full((side, side), 245, np.uint8)
    size, _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 1.2, 3)
    cv2.putText(tile, text, ((side - size[0]) // 2, (side + size[1]) // 2), cv2.FONT_HERSHEY_SIMPLEX, 1.2, 10, 3, cv2.LINE_AA)
    assert digit_count(tile) == len(text)


def test_a_reading_short_of_digits_is_unread(tmp_path):
    fake = tmp_path / "tesseract"
    fake.write_text("#!/bin/sh\ncat > /dev/null\n"
                    "printf 'level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext\\n"
                    "5\\t1\\t1\\t1\\t1\\t1\\t0\\t0\\t10\\t10\\t96\\t2\\n'\n")
    os.chmod(fake, 0o755)
    cam = SyntheticCamera(width=512, vehicles=[(1, 1200, 23)])
    assert Reader(binary=str(fake)).read(cam.render(1, 1200)) == (0, 0, "")  # "2" of "23"
