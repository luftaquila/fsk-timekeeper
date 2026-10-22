"""Protocol lines, frame numbering, the line ring, the detector, clips and settings."""

import json
import os

import numpy as np
import pytest

from fsk_timekeeper import protocol
from fsk_timekeeper.camera import SyntheticCamera
from fsk_timekeeper.clips import ClipStore
from fsk_timekeeper.config import CameraConfig
from fsk_timekeeper.detect import Detector
from fsk_timekeeper.frames import Broken, Frame, FrameTracker
from fsk_timekeeper.ring import LineRing


def test_protocol_lines():
    assert protocol.start(400) == "V S 400"
    assert protocol.stop() == "V S 0"
    assert protocol.crossing(3, 120000, 23, 91) == "V C 3 120000 23 91"
    assert protocol.loss(4, 1, 4000) == "V L 4 1 4000"
    assert protocol.checkpoint(5, 18446744073709551614) == "V K 5 18446744073709551614"
    assert protocol.period_for(40000) == 400
    assert protocol.period_for(50000) == 320
    assert protocol.period_for(60000) == protocol.PERIOD_MIN
    assert protocol.period_for(500) == protocol.PERIOD_MAX
    assert protocol.camera_node("0123ABCD89ef4567") == "89EF4567"
    assert protocol.camera_node("0123ABCD00000000") is None
    assert protocol.is_key_command("K 0123") and protocol.is_key_command("?KEY")
    assert not protocol.is_key_command("C 1 2 ABCDEF01") and not protocol.is_key_command("?ID")
    assert protocol.Line("A V OK").camera_reply() == "ok"
    assert protocol.Line("X camwait").camera_reply() == "camwait"
    assert protocol.Line("X badcmd").camera_reply() is None
    assert protocol.Line("A CP OK").camera_reply() is None
    ident = protocol.Line("I FSK-WL 8 v1.0.0 0123ABCD89EF4567 M 17 922900 7 250.00 16000 0 12 1").identity()
    assert ident == (8, "0123ABCD89EF4567", "M")
    d = "D 89EF4567 OK 0 0 0 830 0.00 0.00 412 0 0 0 1 1 1 XTAL 0 2 1 0 0 0 0 0 0 42 11 0 0 0 0 1"
    assert protocol.Line(d).diag_session("89EF4567") == ("42", "OK")
    assert protocol.Line(d).diag_session("0D2243B0") is None


def frame(fid, ts, h=10, w=4):
    return Frame(fid, ts, np.full((h, w), fid % 250, np.uint8))


def test_frames_number_lines_and_report_gaps():
    period = 400
    ns = 10 * period * 1e9 / 16e6  # 10 lines a frame
    t = FrameTracker(10, period, first_id=1)
    assert t.feed(frame(1, 1000)).first == 1
    b = t.feed(frame(2, int(1000 + ns)))
    assert (b.first, b.lost) == (11, [])
    b = t.feed(frame(5, int(1000 + 4 * ns)))  # 3 and 4 lost in transit
    assert (b.first, b.lost) == (41, [(21, 40)])
    with pytest.raises(Broken):
        t.feed(frame(6, int(1000 + 7 * ns)))  # two frames of time for one id: dropped in the camera
    first = FrameTracker(10, period, first_id=1).feed(frame(3, 0))
    assert (first.first, first.lost) == (21, [(1, 20)])  # the first frames never arrived
    unknown = FrameTracker(10, period).feed(frame(65401, 0))
    assert (unknown.first, unknown.lost) == (1, [])  # ids continue from an earlier acquisition


def test_frames_16_bit_ids_wrap():
    t = FrameTracker(10, 400, first_id=1)
    t.feed(frame(65534, 0))
    assert t.feed(frame(65535, 0)).first == 65535 * 10 - 9
    assert t.feed(frame(1, 0)).first == 65536 * 10 - 9  # 0 is skipped
    with pytest.raises(Broken):
        t.feed(frame(1, 0))


def test_ring_keeps_the_newest_lines():
    ring = LineRing(10, 2)
    ring.write(1, np.arange(7).repeat(2).reshape(7, 2).astype(np.uint8))
    ring.write(8, np.arange(100, 108).repeat(2).reshape(8, 2).astype(np.uint8))
    assert ring.oldest == 6 and ring.newest == 15
    assert ring.read(5, 6) is None and ring.read(14, 16) is None
    assert ring.read(6, 9)[:, 0].tolist() == [5, 6, 100, 101]


def lines_of(cam, count):
    """The synthetic camera's lines 1..count as one block."""
    return cam.render(1, count)


def run_detector(d, lines, block=1000):
    events = []
    for first in range(1, len(lines) + 1, block):
        events += d.process(first, lines[first - 1: first - 1 + block])
    return events


def test_detector_finds_fronts_ignores_light_changes():
    cam = SyntheticCamera(width=256, vehicles=[(6000, 2500, 1), (12000, 2000, 2), (20000, 2000, 3)])
    lines = lines_of(cam, 24000).astype(np.float32)
    lines[9000:9600] *= 0.7  # a shadow over the whole band
    lines[10000:10600, 128:] *= 0.5  # a shadow over half of it
    lines[15000:] *= 1.5  # the sun comes out while the last vehicle is leaving, and stays
    d = Detector(256, enter_lines=80, exit_lines=1200, init_lines=4000)
    events = run_detector(d, lines.clip(0, 255).astype(np.uint8))
    assert [e[1] for e in events if e[0] == "enter"] == [6000, 12000, 20000]
    exits = [(e[1], e[2], e[3]) for e in events if e[0] == "exit"]
    assert exits == [(6000, 8499, False), (12000, 13999, False), (20000, 21999, False)]


def test_detector_tells_a_flat_panel_from_a_shadow():
    rng = np.random.default_rng(3)
    asphalt = 110 + rng.normal(0, 8, 256)
    lines = asphalt + rng.normal(0, 2, (12000, 256))
    lines[6000:7000, 96:160] = 66 + rng.normal(0, 2, (1000, 64))  # a flat panel, as dark as the shadow
    lines[9000:10000, 96:160] *= 0.6  # a shadow of the same size
    d = Detector(256, enter_lines=80, exit_lines=500, init_lines=4000)
    events = run_detector(d, lines.clip(0, 255).astype(np.uint8))
    assert [e[1] for e in events if e[0] == "enter"] == [6001]
    assert [(e[1], e[2]) for e in events if e[0] == "exit"] == [(6001, 7000)]


def test_detector_after_a_gap_suppresses_a_vehicle_in_view():
    cam = SyntheticCamera(width=256, vehicles=[(6000, 3000, 1)])
    lines = lines_of(cam, 12000)
    d = Detector(256, enter_lines=80, exit_lines=500, init_lines=4000)
    d.process(1, lines[:5000])
    d.gap()  # lines 5001..6999 lost; the vehicle arrived in the gap
    events = d.process(7000, lines[6999:12000])
    assert ("exit", 7000, 8999, True) in events
    assert not [e for e in events if e[0] == "enter"]


def test_clips_store_tiles_preview_and_commit(tmp_path):
    store = ClipStore(str(tmp_path), quota_bytes=10**9)
    lines = (np.arange(2500 * 64) % 251).astype(np.uint8).reshape(2500, 64)
    store.write("42", "pending-100", 90, lines, {"line": 100, "ocr": {"car": 7, "conf": 90, "text": "7"}})
    store.commit("42", "pending-100", 3)
    meta = store.meta("42", "3")
    assert (meta["seq"], meta["first"], meta["last"], meta["line"], meta["height"], meta["tiles"]) == (3, 90, 2589, 100, 64, 3)
    assert meta["preview"]["linesPerPixel"] == 2
    assert store.file("42", "3", "2.png") and store.file("42", "3", "preview.webp")
    assert store.file("42", "3", "3.png") is None and store.meta("42", "9") is None
    store.write("43", "pending-1", 1, lines, {"line": 1})
    store.quota = 1
    store.prune()
    assert store.meta("42", "3") is None and os.path.isdir(store.path("43", "pending-1"))  # the newest stays


def test_camera_config_validates_and_persists(tmp_path):
    path = str(tmp_path / "config.json")
    cfg = CameraConfig(path)
    assert cfg.update({"lineRateHz": 50000, "threshold": 5}) == ["lineRateHz", "threshold"]
    with pytest.raises(ValueError):
        cfg.update({"lineRateHz": 59000})
    with pytest.raises(ValueError):
        cfg.update({"bandTop": 3000, "bandBottom": 2000})
    with pytest.raises(ValueError):
        cfg.update({"frameLines": 10})  # not editable
    with pytest.raises(ValueError):
        cfg.update({"travel": "up"})
    with pytest.raises(ValueError):
        cfg.update({"lineRateHz": 58000, "exposureUs": 20})  # 17.2 us a line
    assert cfg.update({"travel": "right", "clipQuotaGb": 2}) == ["travel", "clipQuotaGb"]
    assert cfg.clip_quota_bytes == 2 * 10**9
    assert cfg.editable()["limits"]["travel"] == {"default": "left", "choices": ["left", "right"]}
    assert cfg["bandTop"] == 0  # a refused patch changes nothing
    assert json.load(open(path))["lineRateHz"] == 50000
    assert CameraConfig(path)["threshold"] == 5
    assert cfg.update({"threshold": 5}) == []
