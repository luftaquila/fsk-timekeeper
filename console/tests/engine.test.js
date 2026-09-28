import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { createRun, evaluateRun, invalidateRun, closeRun, shouldInvalidateOnMasterBoot, EngineError } from "../src/lib/engine.js";

const MS = 16000n;
const T0 = 1_000_000_000n; // boundary tick
const NOW = 1_700_000_000_000;
const tick = (ms) => String(T0 + BigInt(ms) * MS);

// Synthetic event log in the shape eventLog stores rows (seq = insertion order).
function makeLog() {
  const rows = [];
  const seqs = {};
  const base = (node, ms, extra) => ({
    seq: rows.length + 1,
    node_id: node,
    ev_seq: rows.length,
    master_tick: tick(ms),
    end_tick: tick(ms),
    master_boot_id: 1,
    sensor_boot_id: 100,
    sync_age_ms: 100,
    received_at: NOW + ms,
    ...extra,
  });
  const api = {
    rows,
    capture(node, ms, extra = {}) {
      seqs[node] = (seqs[node] || 0) + 1;
      const row = base(node, ms, { flags: 15, capture_seq: seqs[node], end_seq: seqs[node], ...extra });
      rows.push(row);
      return row;
    },
    checkpoint(node, ms, extra = {}) {
      const seq = seqs[node] || 0;
      const row = base(node, ms, { flags: 47, capture_seq: seq, end_seq: seq, ...extra });
      rows.push(row);
      return row;
    },
    loss(node, ms, count = 1) {
      const from = (seqs[node] || 0) + 1;
      seqs[node] = from + count - 1;
      const row = base(node, ms, { flags: 31, capture_seq: from, end_seq: seqs[node] });
      rows.push(row);
      return row;
    },
    since(seq) {
      return rows.filter((r) => r.seq > seq);
    },
    findCheckpoint(node, masterBootId, maxTick) {
      return [...rows].reverse().find((r) => r.node_id === node && r.master_boot_id === masterBootId && r.flags & 32 && BigInt(r.master_tick) <= BigInt(maxTick)) || null;
    },
  };
  return api;
}

function start(log, mode, mappings, { now = NOW, boot = 100, lapTarget = null } = {}) {
  return createRun({
    mode,
    note: "car 7",
    clock: { master_tick: String(T0), master_boot_id: 1 },
    mappings,
    findCheckpoint: log.findCheckpoint,
    currentSensorBoot: () => boot,
    lastSeq: log.rows.length,
    lapTarget,
    now,
  });
}

const SPRINT = [{ node_id: "A", role: "start" }, { node_id: "B", role: "finish" }];
const LAPS = [{ node_id: "A", role: "start" }];

function sprintLog() {
  const log = makeLog();
  log.checkpoint("A", -100);
  log.checkpoint("B", -100);
  return log;
}
function lapsLog() {
  const log = makeLog();
  log.checkpoint("A", -100);
  return log;
}

describe("createRun", () => {
  it("snapshots the capture frontier from each mapped node's latest healthy checkpoint", () => {
    const log = makeLog();
    log.capture("A", -5000);
    log.checkpoint("A", -100);
    log.checkpoint("B", -100);
    const run = start(log, "sprint", SPRINT);
    assert.deepEqual(run.nodes, { A: { boot: 100, seq: 1, role: "start" }, B: { boot: 100, seq: 0, role: "finish" } });
    assert.equal(run.cursor, 2); // just before A's checkpoint (min of the two)
    assert.equal(run.armed, true);
    assert.equal(run.verification, "pending");
    assert.equal(run.boundaryTick, String(T0));
    assert.equal(run.lapTarget, null);
    assert.equal("detail" in run, false);
  });

  it("keeps the lap target only for the laps mode", () => {
    assert.equal(start(lapsLog(), "laps", LAPS, { lapTarget: 4 }).lapTarget, 4);
    assert.equal(start(lapsLog(), "laps", LAPS, { lapTarget: 0 }).lapTarget, null);
    assert.equal(start(lapsLog(), "laps", LAPS).lapTarget, null);
    assert.equal(start(sprintLog(), "sprint", SPRINT, { lapTarget: 4 }).lapTarget, null);
  });

  it("freezes the GPS calibration into the run and applies it to results", () => {
    const calibration = { ppb: 100000, ppsTick: String(T0 - 1000n), utc: 1727500000, fix: 1, sats: 9, span: 64 };
    const log = sprintLog();
    const run = createRun({
      mode: "sprint", clock: { master_tick: String(T0), master_boot_id: 1 }, mappings: SPRINT,
      findCheckpoint: log.findCheckpoint, currentSensorBoot: () => 100, lastSeq: log.rows.length, calibration, now: NOW,
    });
    assert.deepEqual(run.calib, calibration);
    log.capture("A", 1000);
    log.capture("B", 61000);
    log.checkpoint("A", 62000);
    log.checkpoint("B", 62000);
    assert.equal(evaluateRun(run, log.since(run.cursor), 300).result, 59994);
    assert.equal(evaluateRun({ ...run, calib: null }, log.since(run.cursor), 300).result, 60000);
    assert.equal(start(sprintLog(), "sprint", SPRINT).calib, null);
  });

  it("refuses to start without a fresh healthy checkpoint from the current boot", () => {
    const log = makeLog();
    assert.throws(() => start(log, "sprint", SPRINT), (e) => e instanceof EngineError && e.code === "checkpoint" && e.node === "A");
    log.checkpoint("A", -100);
    log.checkpoint("B", -100);
    assert.throws(() => start(log, "sprint", SPRINT, { now: NOW + 13000 }), /has not confirmed/); // stale > 12 s
    assert.throws(() => start(log, "sprint", SPRINT, { boot: 101 }), /has not confirmed/); // rebooted since
    const unhealthy = makeLog();
    unhealthy.checkpoint("A", -100, { flags: 46 });
    assert.throws(() => start(unhealthy, "laps", LAPS), /has not confirmed/);
    const future = makeLog();
    future.checkpoint("A", 100); // after the boundary
    assert.throws(() => start(future, "laps", LAPS), /has not confirmed/);
  });
});

describe("evaluateRun — sprint", () => {
  it("times the first finish minus the first start, pending until checkpoints confirm", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.capture("B", 61000);
    let r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.result, null);
    assert.equal(r.run.verification, "pending");
    log.checkpoint("A", 62000);
    log.checkpoint("B", 62000);
    r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.result, 60000);
    assert.equal(r.complete, true);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.armed, false);
    assert.equal(r.accepted.length, 2);
    assert.equal("detail" in r, false);
  });

  it("ignores edges before the boundary and rows from another master session", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", -50); // pre-boundary
    log.capture("A", 500);
    const b = log.capture("B", 900);
    // A stale copy of the same capture under another master session is ignored outright.
    log.rows.push({ ...b, seq: log.rows.length + 1, master_tick: tick(700), end_tick: tick(700), master_boot_id: 2 });
    log.checkpoint("A", 1000);
    log.checkpoint("B", 1000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.result, 400);
    assert.deepEqual(r.accepted.map((e) => e.node_id), ["A", "B"]);
    // A checkpoint from another master session cannot confirm anything either.
    const log2 = sprintLog();
    const run2 = start(log2, "sprint", SPRINT);
    log2.capture("A", 500);
    log2.capture("B", 900);
    log2.checkpoint("A", 1000, { master_boot_id: 2 });
    log2.checkpoint("B", 1000, { master_boot_id: 2 });
    assert.equal(evaluateRun(run2, log2.since(run2.cursor), 300).result, null);
  });

  it("accepts a finish that arrives before the start (order by capture tick, not arrival)", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("B", 5000);
    log.capture("A", 1000);
    log.checkpoint("A", 6000);
    log.checkpoint("B", 6000);
    assert.equal(evaluateRun(run, log.since(run.cursor), 300).result, 4000);
  });

  it("only the first interval counts; later crossings are ignored", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.capture("B", 3000);
    log.capture("A", 5000);
    log.capture("B", 9000);
    log.checkpoint("A", 10000);
    log.checkpoint("B", 10000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.result, 2000);
    assert.equal(r.accepted.length, 4);
  });

  it("invalidates a run whose raw finish−start is not positive", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("B", 1000);
    log.capture("A", 1000);
    log.checkpoint("A", 2000);
    log.checkpoint("B", 2000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.invalidDuration, true);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.fault.kind, "measurement");
    assert.match(r.run.fault.reasons[0].reason, /not positive/);
  });

  it("collapses bounce edges inside the debounce window on raw ticks", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.capture("A", 1065);
    log.capture("A", 1139);
    log.capture("B", 3000);
    log.capture("B", 3299); // 299 ms later: dropped at 300 ms window
    log.checkpoint("A", 4000);
    log.checkpoint("B", 4000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.accepted.length, 2);
    assert.equal(r.result, 2000);
    const r0 = evaluateRun(run, log.since(run.cursor), 0);
    assert.equal(r0.accepted.length, 5);
  });

  it("a capture loss inside the run invalidates it while awaiting evidence", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.loss("B", 2000);
    log.checkpoint("A", 3000);
    log.checkpoint("B", 3000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.fault.node_id, "B");
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.closed, false); // awaitEvidence
    assert.equal(r.run.armed, false);
  });

  it("a master clock fault or a sensor reboot ends the session", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.rows.push({ seq: log.rows.length + 1, node_id: "0", ev_seq: 9, master_tick: tick(1500), end_tick: tick(1500), flags: 16, master_boot_id: 1, sensor_boot_id: 1, capture_seq: 0, end_seq: 0, sync_age_ms: 0, received_at: NOW });
    let r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.fault.node_id, "0");
    assert.equal(r.run.verification, "invalid");

    const log2 = sprintLog();
    const run2 = start(log2, "sprint", SPRINT);
    log2.checkpoint("B", 500, { sensor_boot_id: 101 });
    r = evaluateRun(run2, log2.since(run2.cursor), 300);
    assert.equal(r.fault.node_id, "B");
    assert.match(r.fault.reason, /rebooted/);
  });
});

describe("evaluateRun — laps", () => {
  it("result is the running sum of laps and never completes without a target", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    for (const ms of [1000, 16000, 31000, 46000]) log.capture("A", ms);
    log.checkpoint("A", 47000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.laps.length, 3);
    assert.equal(r.result, 45000);
    assert.equal(r.complete, false);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, false);
    assert.equal(r.run.armed, true);
    const stopped = closeRun(r.run);
    assert.equal(stopped.closed, true);
    assert.equal(stopped.armed, false);
    assert.equal(stopped.result, 45000);
  });

  it("auto-stops at the lap target and ignores later crossings", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS, { lapTarget: 4 });
    for (const ms of [1000, 6000, 11500, 16500, 21000, 26000, 31000]) log.capture("A", ms);
    log.checkpoint("A", 32000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.deepEqual(r.laps.map(Number), [5000 * 16000, 5500 * 16000, 5000 * 16000, 4500 * 16000]);
    assert.equal(r.result, 20000);
    assert.equal(r.complete, true);
    assert.equal(r.run.closed, true);
    assert.equal(r.run.armed, false);
    assert.equal(r.run.verification, "verified");
  });

  it("stays pending and armed below the lap target", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS, { lapTarget: 4 });
    for (const ms of [1000, 6000, 11000]) log.capture("A", ms);
    log.checkpoint("A", 12000);
    const r = evaluateRun(run, log.since(run.cursor), 300);
    assert.equal(r.laps.length, 2);
    assert.equal(r.result, 10000); // running sum is shown, but not final
    assert.equal(r.complete, false);
    assert.equal(r.run.armed, true);
    assert.equal(r.run.verification, "verified");
  });

  it("a non-positive lap invalidates the run (two start sensors crossing on the same tick)", () => {
    const log = makeLog();
    log.checkpoint("A", -100);
    log.checkpoint("B", -100);
    const run = start(log, "laps", [{ node_id: "A", role: "start" }, { node_id: "B", role: "start" }]);
    log.capture("A", 1000);
    log.capture("B", 1000);
    log.checkpoint("A", 2000);
    log.checkpoint("B", 2000);
    const r = evaluateRun(run, log.since(run.cursor), 0);
    assert.equal(r.invalidDuration, true);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.closed, true);
  });

  it("equal ticks on one sensor are a capture-order fault, caught before the lap math", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    log.capture("A", 1000);
    log.capture("A", 1000);
    log.checkpoint("A", 2000);
    const r = evaluateRun(run, log.since(run.cursor), 0);
    assert.equal(r.invalidDuration, false);
    assert.equal(r.fault?.node_id, "A");
    assert.equal(r.run.verification, "invalid");
  });
});

describe("run state helpers", () => {
  it("invalidateRun keeps the first fault and disarms", () => {
    const run = { mode: "sprint", runId: "r", armed: true, closed: false, verification: "pending", fault: null };
    const a = invalidateRun(run, [{ node_id: "0", reason: "x" }], { awaitEvidence: true });
    assert.equal(a.armed, false);
    assert.equal(a.closed, false);
    assert.equal(a.fault.reasons[0].reason, "x");
    const b = invalidateRun(a, [{ node_id: "0", reason: "y" }]);
    assert.equal(b.fault.reasons[0].reason, "x");
    assert.equal(b.closed, true);
  });
  it("master boot change only matters for an armed open run", () => {
    const run = { armed: true, closed: false, masterBootId: 1 };
    assert.equal(shouldInvalidateOnMasterBoot(run, 2), true);
    assert.equal(shouldInvalidateOnMasterBoot(run, 1), false);
    assert.equal(shouldInvalidateOnMasterBoot({ ...run, armed: false }, 2), false);
    assert.equal(shouldInvalidateOnMasterBoot(null, 2), false);
  });
});
