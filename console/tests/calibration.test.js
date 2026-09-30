import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { buildTimeline, timeAt, calibrationPoints, durationNs, pointsMethod, freezeCalibration, utcMsAt } from "../src/lib/calibration.js";
import { resultNs, lapsNs } from "../src/lib/results.js";

const F = 16_000_000;
const BASE = 5_000_000_000n;

// Oscillator whose frequency ramps linearly (a warming crystal): ticks(t) = F·(t + a·t + b·t²/2).
function oscillator({ a = 20e-6, b = 1e-7 } = {}) {
  return (t) => BASE + BigInt(Math.round(F * (t + a * t + (b * t * t) / 2)));
}

function edgesOf(ticksAt, seconds, { seg = 1, utc0 = 1_800_000_000, n0 = 0, drop = new Set() } = {}) {
  const out = [];
  for (const s of seconds) {
    if (drop.has(s)) continue;
    out.push({ tick: String(ticksAt(s)), seg, n: n0 + (s - seconds[0]), utc: utc0 == null ? null : utc0 + s });
  }
  return out;
}

const range = (from, to) => Array.from({ length: to - from + 1 }, (_, i) => from + i);
const seconds = (timeline, tick) => {
  const { t } = timeAt(timeline, tick);
  return Number(t.n) / Number(t.d);
};

describe("PPS timeline", () => {
  it("interpolates within a segment to < 100 ns while the frequency ramps", () => {
    const osc = oscillator();
    const timeline = buildTimeline(edgesOf(osc, range(0, 120)));
    let worst = 0;
    for (let i = 0; i < 500; i++) {
      const t = 0.5 + Math.random() * 119;
      const err = Math.abs(seconds(timeline, osc(t)) - (t - 0)) * 1e9;
      worst = Math.max(worst, err);
    }
    assert.ok(worst < 100, `worst interpolation error ${worst} ns`);
  });

  it("an interval between two ticks matches the true GPS time to < 100 ns", () => {
    const osc = oscillator({ a: -35e-6, b: 2e-7 });
    const edges = edgesOf(osc, range(0, 300));
    const a = osc(12.345678);
    const b = osc(287.654321);
    const ns = durationNs(calibrationPoints(edges, [a, b]), a, b);
    const truth = (287.654321 - 12.345678) * 1e9;
    assert.ok(Math.abs(Number(ns) - truth) < 100, `${ns} vs ${truth}`);
    assert.equal(pointsMethod(calibrationPoints(edges, [a, b])), "gps");
  });

  it("dropped P lines only widen the interpolation step (Δn keeps the seconds exact)", () => {
    const osc = oscillator();
    const edges = edgesOf(osc, range(0, 60), { drop: new Set([10, 20, 21, 35]) });
    const timeline = buildTimeline(edges);
    for (const t of [10.4, 20.7, 34.9, 50.5]) assert.ok(Math.abs(seconds(timeline, osc(t)) - t) * 1e9 < 100, `t=${t}`);
  });

  it("bridges a segment change with the UTC seconds between them, or by rounding within an hour", () => {
    const osc = oscillator({ b: 0 });
    const first = edgesOf(osc, range(0, 20), { seg: 1 });
    const second = edgesOf(osc, range(40, 60), { seg: 2, n0: 0 });
    for (const utc of [true, false]) {
      const edges = [...first, ...second].map((e) => (utc ? e : { ...e, utc: null }));
      const timeline = buildTimeline(edges);
      assert.equal(timeline.segs[1].island, 0);
      const inGap = osc(30.25);
      const at = timeAt(timeline, inGap);
      assert.equal(at.how, "bridge");
      assert.ok(Math.abs(seconds(timeline, inGap) - 30.25) * 1e9 < 1000);
      const a = osc(5);
      const b = osc(55);
      assert.ok(Math.abs(Number(durationNs(calibrationPoints(edges, [a, b]), a, b)) - 50e9) < 100);
    }
  });

  it("does not bridge edges that disagree with the crystal (a PPS phase step between segments)", () => {
    const osc = oscillator({ b: 0 });
    const step = BigInt(F / 20); // the second segment's pulses come 50 ms late
    const first = edgesOf(osc, range(0, 20), { seg: 1 });
    const second = edgesOf(osc, range(40, 60), { seg: 2, n0: 0 }).map((e) => ({ ...e, tick: String(BigInt(e.tick) + step) }));
    for (const utc of [true, false]) {
      const edges = [...first, ...second].map((e) => (utc ? e : { ...e, utc: null }));
      const timeline = buildTimeline(edges);
      assert.equal(timeline.segs[1].island, 1);
      assert.equal(timeAt(timeline, osc(30.25)).how, "extrap");
      // across the gap the crystal counts, not the stepped PPS
      const a = osc(5);
      const b = osc(55);
      const points = calibrationPoints(edges, [a, b]);
      assert.ok(Math.abs(Number(durationNs(points, a, b)) - 50e9) < 1000, `${durationNs(points, a, b)}`);
      assert.equal(pointsMethod(points), "gps-extrapolated");
    }
  });

  it("checks a bridge against the neighbours' frequencies to the microsecond", () => {
    const stepped = (osc, from, to, stepUs) =>
      edgesOf(osc, range(from, to), { seg: 2, n0: 0 }).map((e) => ({ ...e, tick: String(BigInt(e.tick) + BigInt(Math.round((stepUs * F) / 1e6))) }));
    const island = (edges) => buildTimeline(edges).segs[1].island;
    const steady = oscillator({ b: 0 });
    // a 100 us PPS step across a 20 s gap: 200 ppm would allow 4 ms, the neighbours 21 us
    assert.equal(island([...edgesOf(steady, range(0, 60)), ...stepped(steady, 80, 140, 100)]), 1);
    assert.equal(island([...edgesOf(steady, range(0, 60)), ...stepped(steady, 80, 140, 15)]), 0);
    // a crystal warming 0.1 ppm/s over a 600 s gap is followed, not mistaken for a step
    const warming = oscillator({ a: 20e-6, b: 1e-7 });
    assert.equal(island([...edgesOf(warming, range(0, 60)), ...stepped(warming, 660, 720, 0)]), 0);
    // one edge on a side gives no frequency: only the 200 ppm gate applies
    assert.equal(island([...edgesOf(steady, [0]), ...stepped(steady, 20, 80, 1000)]), 0);
  });

  it("extrapolates past the last edge with the nearest segment's frequency", () => {
    const osc = oscillator({ a: 50e-6, b: 0 });
    const edges = edgesOf(osc, range(0, 100));
    const timeline = buildTimeline(edges);
    const beyond = osc(101.5);
    const at = timeAt(timeline, beyond);
    assert.equal(at.how, "extrap");
    assert.equal(at.extrapTicks, beyond - BigInt(edges[edges.length - 1].tick));
    assert.ok(Math.abs(seconds(timeline, beyond) - 101.5) * 1e9 < 100);
    assert.equal(pointsMethod(calibrationPoints(edges, [osc(50), beyond])), "gps-extrapolated");
  });

  it("separates islands beyond an hour without UTC and keeps T continuous at the midpoint", () => {
    const osc = oscillator({ a: 10e-6, b: 0 });
    const edges = [...edgesOf(osc, range(0, 10), { seg: 1, utc0: null }), ...edgesOf(osc, range(8000, 8010), { seg: 2, utc0: null })];
    const timeline = buildTimeline(edges);
    assert.equal(timeline.segs[1].island, 1);
    const mid = (BigInt(edges[10].tick) + BigInt(edges[11].tick)) / 2n;
    const left = timeAt(timeline, mid);
    const right = timeAt(timeline, mid + 1n);
    assert.equal(left.how, "extrap");
    assert.ok(seconds(timeline, mid + 1n) - seconds(timeline, mid) < 1e-6);
    assert.ok(Math.abs(seconds(timeline, osc(8005)) - seconds(timeline, osc(5)) - 8000) < 1e-3);
    assert.equal(right.how, "extrap");
  });

  it("no qualified edge: nominal 16 MHz", () => {
    const points = calibrationPoints([], ["16000000", "48000001"]);
    assert.equal(points["16000000"].how, "nominal");
    assert.equal(pointsMethod(points), "nominal");
    assert.equal(durationNs(points, "16000000", "48000001"), 2_000_000_063n); // 32 000 001 ticks × 62.5 ns, half up
  });

  it("a frozen calibration is what results use, whatever edges arrive later", () => {
    const osc = oscillator();
    const early = edgesOf(osc, range(0, 30));
    const a = String(osc(3.2));
    const b = String(osc(33.7)); // beyond the edges at freeze time: extrapolated
    const calibration = freezeCalibration(early, [a, b], 1);
    const run = { schema: 3, mode: "sprint", startTick: a, finishTick: b, calibration, durationNs: null };
    const frozen = resultNs(run, buildTimeline(early));
    const later = buildTimeline(edgesOf(osc, range(0, 60)));
    assert.equal(resultNs(run, later), frozen);
    assert.equal(calibration.method, "gps-extrapolated");
    const laps = { schema: 3, mode: "laps", crossingTicks: [a, b], totalValid: true, calibration };
    assert.deepEqual(lapsNs(laps, later), [frozen]);
  });

  it("dates a tick from the latest edge that carries UTC", () => {
    const osc = oscillator({ a: 0, b: 0 });
    const edges = edgesOf(osc, range(0, 10), { utc0: 1_800_000_000 });
    assert.equal(utcMsAt(edges, osc(10.25)), 1_800_000_010_250);
    assert.equal(utcMsAt(edges.map((e) => ({ ...e, utc: null })), osc(5)), null);
  });
});
