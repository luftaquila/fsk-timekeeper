// Camera modes: camera-sprint with operator decisions, camera-laps heats of several cars.
import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { createRun, evaluateRun, stopRun, isRecord } from "../src/lib/engine.js";
import { crossingKey, lineTick, mergeDecision, decidedTick } from "../src/lib/camera.js";
import { carRun, lapsNs, resultNs, heldLaps } from "../src/lib/results.js";
import { extendCalibration, freezeCalibration, durationNs } from "../src/lib/calibration.js";

const MS = 16000n;
const T0 = 1_000_000_000n;
const NOW = 1_700_000_000_000;
const tick = (ms) => String(T0 + BigInt(ms) * MS);
const PERIOD = 400; // 40 kHz lines: 25 us

function makeLog() {
  const rows = [];
  const seqs = {};
  const base = (node, extra) => ({ seq: rows.length + 1, node_id: node, ev_seq: rows.length, master_boot_id: 1,
    sensor_boot_id: 100, sync_age_ms: 0, received_at: NOW, flags: 7, ...extra });
  const api = {
    rows,
    capture(node, ms, extra = {}) {
      seqs[node] = (seqs[node] || 0) + 1;
      const row = base(node, { kind: "capture", capture_seq: seqs[node], end_seq: seqs[node], master_tick: tick(ms), end_tick: tick(ms), ...extra });
      rows.push(row);
      return row;
    },
    // A camera crossing first seen on `line` at ms, its run of lines 1000 either side.
    camera(node, ms, { line = 100000 + ms * 40, car = 0, conf = 0 } = {}) {
      return api.capture(node, ms, { camera: { line: String(line), period: PERIOD, run_lo: String(line - 1000), run_hi: String(line + 1000), car, conf } });
    },
    checkpoint(node, ms) {
      const seq = seqs[node] || 0;
      rows.push(base(node, { kind: "checkpoint", capture_seq: seq, end_seq: seq, master_tick: tick(ms), end_tick: tick(ms) }));
    },
    loss(node, ms, endMs) {
      seqs[node] = (seqs[node] || 0) + 1;
      rows.push(base(node, { kind: "loss", capture_seq: seqs[node], end_seq: seqs[node], master_tick: tick(ms), end_tick: tick(endMs) }));
    },
    since: (seq) => rows.filter((r) => r.seq > seq),
    findCheckpoint: (node, boot, maxTick) => [...rows].reverse().find((r) => r.node_id === node && r.kind === "checkpoint" && BigInt(r.master_tick) <= BigInt(maxTick)) || null,
  };
  return api;
}

function start(log, mode, mappings, extra = {}) {
  return createRun({ mode, clock: { master_tick: String(T0), master_boot_id: 1 }, mappings, findCheckpoint: log.findCheckpoint,
    currentSensorBoot: () => 100, lastSeq: log.rows.length, debounceMs: 300, now: NOW, cameraNode: "CA", ...extra });
}
const ev = (run, log) => evaluateRun(run, log.since(run.cursor), { now: NOW });
const decide = (run, row, patch) => ({ ...run, decisions: { ...run.decisions, [crossingKey(row)]: mergeDecision(row, run.decisions?.[crossingKey(row)], patch, NOW) } });

describe("camera crossing lines", () => {
  it("maps lines of the crossing's run to ticks and nothing outside it", () => {
    const row = makeLog().camera("CA", 10, { line: 5000 });
    assert.equal(lineTick(row, 5000), BigInt(tick(10)));
    assert.equal(lineTick(row, 5040), BigInt(tick(11))); // 40 lines of 25 us
    assert.equal(lineTick(row, 4001), BigInt(tick(10)) - 999n * 400n);
    assert.equal(lineTick(row, 4000), null); // line 3999 lies before the run: no exact time
    assert.equal(lineTick(row, 6001), null);
    assert.throws(() => mergeDecision(row, {}, { line: 4000 }), /no exact time/);
    assert.throws(() => mergeDecision(row, {}, { car: "007" }), /one to three digits/);
    assert.equal(mergeDecision(row, {}, { car: "7" }, 1).car, "7");
    assert.equal(decidedTick(row, { line: "5040" }), BigInt(tick(11)));
  });
});

describe("camera-sprint", () => {
  const MAP = [{ node_id: "A", role: "start" }, { node_id: "CA", role: "finish" }];
  function log2() {
    const log = makeLog();
    log.checkpoint("A", -100);
    log.checkpoint("CA", -100);
    return log;
  }

  it("times a sensor start to the camera finish; the operator's front line moves only the finish", () => {
    const log = log2();
    const run = start(log, "camera-sprint", MAP);
    assert.deepEqual(run.decisions, {});
    assert.equal(run.cameraNode, "CA");
    log.capture("A", 0);
    const finish = log.camera("CA", 4000, { car: 23, conf: 95 });
    let r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.finishTick, tick(4000));
    const shown = r.crossings.find((c) => c.role === "finish");
    assert.equal(shown.key, crossingKey(finish));
    assert.equal(shown.camera.car, 23);
    // The detector caught the shadow 2 ms early: the operator picks the nose.
    r = ev(decide(r.run, finish, { line: Number(finish.camera.line) + 80 }), log);
    assert.equal(r.run.finishTick, tick(4002));
    assert.equal(r.crossings.find((c) => c.role === "finish").autoTick, tick(4000));
    // Clearing the decision restores the detector's line.
    r = ev(decide(r.run, finish, { line: null }), log);
    assert.equal(r.run.finishTick, tick(4000));
  });

  it("an ignored detection is no crossing: the next camera crossing finishes", () => {
    const log = log2();
    let run = start(log, "camera-sprint", MAP);
    log.capture("A", 0);
    const marshal = log.camera("CA", 1500);
    log.camera("CA", 4000);
    let r = ev(run, log);
    assert.equal(r.run.finishTick, tick(1500));
    run = decide(r.run, marshal, { ignore: true });
    r = ev(run, log);
    assert.equal(r.run.finishTick, tick(4000));
    // A car number decision is display only in camera-sprint.
    r = ev(decide(r.run, marshal, { ignore: false, car: "7" }), log);
    assert.equal(r.run.finishTick, tick(1500));
  });

  it("a camera loss below the finish invalidates like a sensor loss", () => {
    const log = log2();
    const run = start(log, "camera-sprint", MAP);
    log.capture("A", 0);
    log.loss("CA", 2000, 2100);
    log.camera("CA", 4000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /^Camera: Capture Loss/);
  });
});

describe("camera-laps heats", () => {
  const MAP = [{ node_id: "CA", role: "start" }];
  function heat(extra = {}) {
    const log = makeLog();
    log.checkpoint("CA", -100);
    return { log, run: start(log, "camera-laps", MAP, { ocrMinConf: 80, ...extra }) };
  }

  it("starts without cars: a car is any number its crossings go to", () => {
    const { log, run } = heat();
    assert.deepEqual(run.results, {});
    assert.equal(run.cars, undefined);
    log.camera("CA", 0, { car: 7, conf: 99 });
    const r = ev(run, log);
    assert.deepEqual(Object.keys(r.run.results), ["7"]);
    assert.equal(r.run.results["7"].crossingTicks.length, 1);
  });

  it("times each car from its own crossings; two cars 100 ms apart both count", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 100, { car: 23, conf: 90 });
    log.camera("CA", 60000, { car: 1, conf: 97 });
    log.camera("CA", 61500, { car: 23, conf: 85 });
    const r = ev(run, log);
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    assert.deepEqual(r.run.results["23"].crossingTicks, [tick(100), tick(61500)]);
    assert.equal(r.run.results["1"].verification, "verified");
    assert.equal(r.run.results["1"].recordedAt, NOW);
    assert.equal(r.run.closed, false);
    assert.deepEqual(r.laps["23"], [{ startTick: tick(100), endTick: tick(61500) }]);
    assert.deepEqual(lapsNs(carRun(r.run, "23")), [61_400_000_000n]);
    assert.ok(isRecord(r.run));
    assert.deepEqual(r.crossings.map((c) => c.state), ["assigned", "assigned", "assigned", "assigned"]);
  });

  it("an unread crossing holds, for every car, the lap it falls in, the lap count and the best lap", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 5000, { car: 23, conf: 99 });
    const unread = log.camera("CA", 60000, { car: 0, conf: 0 });
    log.camera("CA", 65000, { car: 23, conf: 99 });
    log.camera("CA", 70000, { car: 7, conf: 99 });
    log.camera("CA", 130000, { car: 7, conf: 99 });
    let r = ev(run, log);
    assert.equal(r.crossings[2].state, "unassigned");
    for (const car of ["1", "23", "7"]) {
      assert.equal(r.run.results[car].verification, "pending");
      assert.deepEqual(r.run.results[car].heldTicks, [tick(60000)]);
    }
    // The laps' own times stand; only the lap around the unread crossing may be two.
    assert.deepEqual(r.run.results["23"].crossingTicks, [tick(5000), tick(65000)]);
    assert.deepEqual([...heldLaps(carRun(r.run, "23"))], [0]);
    assert.deepEqual(r.run.results["7"].crossingTicks, [tick(70000), tick(130000)]);
    assert.deepEqual([...heldLaps(carRun(r.run, "7"))], []); // it may be car 7's first crossing: one lap more
    assert.equal(resultNs(carRun(r.run, "7")), null);
    r = ev(decide(r.run, unread, { car: "1" }), log);
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    for (const car of ["1", "23", "7"]) {
      assert.equal(r.run.results[car].verification, "verified");
      assert.deepEqual(r.run.results[car].heldTicks, []);
    }
  });

  it("a low-confidence reading is unassigned; ignoring it releases the cars; a misread car goes with its fix", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    const weak = log.camera("CA", 30000, { car: 1, conf: 50 });
    const misread = log.camera("CA", 40000, { car: 77, conf: 99 });
    log.camera("CA", 60000, { car: 1, conf: 99 });
    let r = ev(run, log);
    assert.deepEqual(r.crossings.map((c) => c.state), ["assigned", "unassigned", "assigned", "assigned"]);
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    assert.deepEqual([...heldLaps(carRun(r.run, "1"))], [0]);
    assert.deepEqual(r.run.results["77"].crossingTicks, [tick(40000)]);
    r = ev(decide(r.run, weak, { ignore: true }), log);
    assert.equal(r.run.results["1"].verification, "verified");
    r = ev(decide(r.run, misread, { car: "17" }), log);
    assert.deepEqual(Object.keys(r.run.results), ["1", "17"]);
    assert.deepEqual(r.crossings.map((c) => c.state), ["assigned", "ignored", "assigned", "assigned"]);
  });

  it("takes no debounce: a number read again within it is unassigned, two cars that close both count", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 100, { car: 23, conf: 99 }); // another car 100 ms behind
    const again = log.camera("CA", 200, { car: 1, conf: 99 }); // car 1 cannot pass twice in 200 ms
    log.camera("CA", 60000, { car: 1, conf: 99 });
    let r = ev(run, log);
    assert.deepEqual(r.crossings.map((c) => c.state), ["assigned", "assigned", "unassigned", "assigned"]);
    assert.deepEqual(r.run.results["23"].crossingTicks, [tick(100)]);
    assert.deepEqual([...heldLaps(carRun(r.run, "1"))], [0]);
    r = ev(decide(r.run, again, { ignore: true }), log); // the operator sees a split detection
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    assert.equal(r.run.results["1"].verification, "verified");
  });

  it("Stop makes unresolved unassigned crossings final: the cars they may belong to turn invalid", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 60000, { car: 1, conf: 99 });
    log.camera("CA", 90000, { car: 0, conf: 0 });
    log.camera("CA", 120000, { car: 1, conf: 99 });
    const open = ev(run, log);
    assert.equal(open.run.results["1"].verification, "pending");
    assert.deepEqual([...heldLaps(carRun(open.run, "1"))], [1]);
    const stopped = ev(stopRun(open.run), log);
    assert.equal(stopped.run.closed, true);
    const car = stopped.run.results["1"];
    assert.equal(car.verification, "invalid");
    assert.deepEqual(car.crossingTicks, [tick(0), tick(60000), tick(120000)]); // every lap time stands
    assert.equal(resultNs(carRun(stopped.run, "1")), null); // no total
    assert.match(car.fault.reasons[0].reason, /^Camera: Unassigned Crossing/);
    assert.deepEqual(Object.keys(stopped.run.results), ["1"]);
  });

  it("a camera loss invalidates every open car above it", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 1000, { car: 23, conf: 99 });
    log.camera("CA", 60000, { car: 1, conf: 99 });
    log.loss("CA", 70000, 70100);
    log.camera("CA", 120000, { car: 23, conf: 99 });
    const r = ev(run, log);
    assert.equal(r.run.results["1"].verification, "invalid");
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    assert.equal(r.run.results["23"].verification, "invalid");
    assert.deepEqual(r.run.results["23"].crossingTicks, [tick(1000)]);
  });

  it("a car reaching the lap target is decided; the heat stays open for cars still to come", () => {
    const { log, run } = heat({ lapTarget: 1 });
    assert.equal(run.lapTarget, 1);
    log.camera("CA", 0, { car: 1, conf: 99 });
    log.camera("CA", 60000, { car: 1, conf: 99 });
    log.camera("CA", 61000, { car: 23, conf: 99 });
    let r = ev(run, log);
    assert.equal(r.run.results["1"].closed, true);
    assert.equal(r.run.results["23"].closed, false);
    assert.equal(r.run.closed, false);
    log.camera("CA", 120000, { car: 1, conf: 99 }); // past its target: no lap
    log.camera("CA", 121000, { car: 23, conf: 99 });
    r = ev(r.run, log);
    assert.deepEqual(r.run.results["1"].crossingTicks, [tick(0), tick(60000)]);
    assert.equal(r.run.results["23"].closed, true);
    assert.equal(r.run.closed, false);
    // After its last lap, an unread crossing can no longer be one of car 1's.
    log.camera("CA", 150000, { car: 0, conf: 0 });
    r = ev(r.run, log);
    assert.deepEqual(r.run.results["1"].heldTicks, []);
    assert.equal(r.run.results["1"].verification, "verified");
    assert.equal(ev(stopRun(r.run), log).run.closed, true);
  });

  it("a classified car stays without crossings; classifying never closes the heat", () => {
    const { log, run } = heat();
    log.camera("CA", 0, { car: 1, conf: 99 });
    let r = ev(run, log);
    r = ev({ ...r.run, results: { ...r.run.results, "23": { crossingTicks: [], verification: "pending", classification: "DNS", closed: false,
      recordedAt: null, calibration: null, durationNs: null, fault: null } } }, log);
    assert.equal(r.run.results["23"].classification, "DNS");
    assert.equal(r.run.results["23"].closed, true);
    assert.equal(r.run.closed, false);
    assert.ok(isRecord(r.run));
  });
});

describe("frozen calibration and later decisions", () => {
  it("times a tick brought in after the freeze from the frozen points only", () => {
    // 16 000 010 ticks per GPS second: +0.625 ppm.
    const edges = [0, 1, 2, 3].map((n) => ({ tick: String(16_000_000n + BigInt(n) * 16_000_010n), seg: 1, n }));
    const a = "16000000", b = String(16_000_000n + 2n * 16_000_010n);
    const frozen = freezeCalibration(edges, [a, b]);
    const later = String(BigInt(b) + 16_000_010n); // one GPS second after b
    const extended = extendCalibration(frozen, [a, b, later]);
    assert.equal(Object.keys(extended.points).length, 3);
    assert.equal(durationNs(extended.points, b, later), 1_000_000_000n);
    assert.equal(extendCalibration(extended, [a, later]), extended); // nothing new: unchanged
  });
});
