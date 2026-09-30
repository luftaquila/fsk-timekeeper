import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { createRun, evaluateRun, invalidateRun, stopRun, endMasterSession, shouldInvalidateOnMasterBoot, EngineError, RUN_SCHEMA } from "../src/lib/engine.js";
import { nominalNs } from "../src/lib/event-timing.js";

const MS = 16000n;
const T0 = 1_000_000_000n; // boundary tick
const NOW = 1_700_000_000_000;
const tick = (ms) => String(T0 + BigInt(ms) * MS);
const secs = (a, b) => Number(nominalNs(BigInt(b) - BigInt(a))) / 1e9;

// Synthetic event log in the shape eventLog stores v2 rows (seq = insertion order).
function makeLog() {
  const rows = [];
  const seqs = {};
  const base = (node, extra) => ({
    seq: rows.length + 1,
    node_id: node,
    ev_seq: rows.length,
    master_boot_id: 1,
    sensor_boot_id: 100,
    sync_age_ms: 100,
    received_at: NOW,
    flags: 7,
    ...extra,
  });
  const api = {
    rows,
    capture(node, ms, extra = {}) {
      seqs[node] = (seqs[node] || 0) + 1;
      const row = base(node, { kind: "capture", capture_seq: seqs[node], end_seq: seqs[node], master_tick: tick(ms), end_tick: tick(ms), ...extra });
      rows.push(row);
      return row;
    },
    checkpoint(node, ms, extra = {}) {
      const seq = seqs[node] || 0;
      const row = base(node, { kind: "checkpoint", capture_seq: seq, end_seq: seq, master_tick: tick(ms), end_tick: tick(ms), ...extra });
      rows.push(row);
      return row;
    },
    // Known-time loss of `count` captures in [ms, endMs]; unknown: flags 64.
    loss(node, ms, { count = 1, endMs = ms, unknown = false } = {}) {
      const from = (seqs[node] || 0) + 1;
      seqs[node] = from + count - 1;
      const row = base(node, { kind: "loss", capture_seq: from, end_seq: seqs[node], master_tick: unknown ? "0" : tick(ms), end_tick: unknown ? "0" : tick(endMs), flags: unknown ? 64 : 7 });
      rows.push(row);
      return row;
    },
    skip(node, count = 1) {
      seqs[node] = (seqs[node] || 0) + count;
    },
    quarantine(node = "*") {
      const row = base(node, { kind: "quarantine", node_id: node, hseq: rows.length + 1, raw: "E ..." });
      rows.push(row);
      return row;
    },
    masterEnd(ms) {
      const row = base("0", { kind: "loss", capture_seq: 0, end_seq: 0, master_tick: tick(ms), end_tick: tick(ms), flags: 0, sensor_boot_id: 1 });
      rows.push(row);
      return row;
    },
    since(seq) {
      return rows.filter((r) => r.seq > seq);
    },
    findCheckpoint(node, masterBootId, maxTick) {
      return [...rows].reverse().find((r) => r.node_id === node && r.master_boot_id === masterBootId && r.kind === "checkpoint" && BigInt(r.master_tick) <= BigInt(maxTick)) || null;
    },
  };
  return api;
}

function start(log, mode, mappings, { now = NOW, boot = 100, lapTarget = null, debounceMs = 300 } = {}) {
  return createRun({
    mode,
    note: "car 7",
    clock: { master_tick: String(T0), master_boot_id: 1 },
    mappings,
    findCheckpoint: log.findCheckpoint,
    currentSensorBoot: () => boot,
    lastSeq: log.rows.length,
    lapTarget,
    debounceMs,
    now,
  });
}

const SPRINT = [
  { node_id: "A", role: "start" },
  { node_id: "B", role: "finish" },
];
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
const ev = (run, log) => evaluateRun(run, log.since(run.cursor), { now: NOW });

describe("createRun", () => {
  it("snapshots each mapped sensor's START checkpoint and the protocol versions", () => {
    const log = makeLog();
    log.capture("A", -5000);
    log.checkpoint("A", -100);
    log.checkpoint("B", -100);
    const run = start(log, "sprint", SPRINT);
    assert.deepEqual(run.nodes, { A: { boot: 100, seq: 1, role: "start", cpSeq: 2 }, B: { boot: 100, seq: 0, role: "finish", cpSeq: 3 } });
    assert.equal(run.cursor, 2);
    assert.equal(run.armed, true);
    assert.equal(run.verification, "pending");
    assert.equal(run.boundaryTick, String(T0));
    assert.equal(run.schema, RUN_SCHEMA);
    assert.equal(run.radioProto, 11);
    assert.equal(run.usbProto, 2);
    assert.equal(run.calibration, null);
    assert.equal(run.durationNs, null);
  });

  it("keeps the lap target only for the laps mode and freezes the debounce window", () => {
    assert.equal(start(lapsLog(), "laps", LAPS, { lapTarget: 4 }).lapTarget, 4);
    assert.equal(start(lapsLog(), "laps", LAPS, { lapTarget: 0 }).lapTarget, null);
    assert.equal(start(sprintLog(), "sprint", SPRINT, { lapTarget: 4 }).lapTarget, null);
    assert.equal(start(sprintLog(), "sprint", SPRINT, { debounceMs: 1000 }).debounceMs, 1000);
    assert.equal(start(sprintLog(), "sprint", SPRINT, { debounceMs: -1 }).debounceMs, 300);
    assert.equal(start(sprintLog(), "sprint", SPRINT, { debounceMs: 2.5 }).debounceMs, 300);
  });

  it("refuses to start without a fresh healthy checkpoint from the current boot", () => {
    const log = makeLog();
    assert.throws(() => start(log, "sprint", SPRINT), (e) => e instanceof EngineError && e.code === "checkpoint" && e.node === "A");
    log.checkpoint("A", -100);
    log.checkpoint("B", -100);
    assert.throws(() => start(log, "sprint", SPRINT, { now: NOW + 13000 }), /has not confirmed/);
    assert.throws(() => start(log, "sprint", SPRINT, { boot: 101 }), /has not confirmed/);
    const unhealthy = makeLog();
    unhealthy.checkpoint("A", -100, { flags: 6 });
    assert.throws(() => start(unhealthy, "laps", LAPS), /has not confirmed/);
    const future = makeLog();
    future.checkpoint("A", 100);
    assert.throws(() => start(future, "laps", LAPS), /has not confirmed/);
  });
});

describe("PLAN §2.5 cases (D = 300 ms)", () => {
  it("A: a start-sensor loss after S1 does not matter", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 10);
    log.capture("B", 4010);
    log.loss("A", 3000);
    log.checkpoint("A", 6000);
    log.checkpoint("B", 6000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(secs(r.run.startTick, r.run.finishTick), 4);
    assert.equal(r.run.closed, true);
    assert.equal(r.run.fault, null);
  });

  it("A0: the same run without the loss", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 10);
    log.capture("B", 4010);
    log.checkpoint("A", 6000);
    log.checkpoint("B", 6000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.startTick, tick(10));
    assert.equal(r.run.finishTick, tick(4010));
  });

  it("A1: the two first crossings alone decide it — no checkpoint needed", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 10);
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending");
    assert.equal(r.run.closed, false);
    log.capture("B", 4010);
    r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.armed, false);
    assert.equal(secs(r.run.startTick, r.run.finishTick), 4);
  });

  it("F: a finish before the first start is ignored without a warning", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("B", 500);
    log.capture("A", 1000);
    log.capture("B", 5000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.finishTick, tick(5000));
    assert.equal(secs(r.run.startTick, r.run.finishTick), 4);
    assert.equal(r.run.fault, null);
    assert.deepEqual(
      r.crossings.map((c) => [c.role, c.tick]),
      [
        ["start", tick(1000)],
        ["finish", tick(5000)],
      ],
    );
  });

  it("D: Stop with a start and no finish becomes DNF", () => {
    const log = sprintLog();
    const run = stopRun(start(log, "sprint", SPRINT), tick(5000));
    log.capture("A", 10);
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending");
    log.checkpoint("A", 6000);
    log.checkpoint("B", 6000);
    r = ev(run, log);
    assert.equal(r.run.verification, "dnf");
    assert.equal(r.run.dnfReason, "DNF");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.finishTick, null);
  });

  it("DNS: Stop with no start crossing at all", () => {
    const log = sprintLog();
    const run = stopRun(start(log, "sprint", SPRINT), tick(5000));
    log.capture("B", 1000); // a finish without a start is ignored
    log.checkpoint("A", 6000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "dnf");
    assert.equal(r.run.dnfReason, "DNS");
  });
});

describe("dependency intervals and holes", () => {
  it("a hole straddling the boundary can hide the real S1: invalid", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.loss("A", -100, { endMs: 100 });
    log.capture("A", 500);
    log.capture("B", 3000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.fault.reasons[0].node_id, "A");
    assert.match(r.run.fault.reasons[0].reason, /lost captures/);
    assert.equal(r.run.fault.reasons[0].lo, String(BigInt(tick(-100)) - 1n)); // the hole itself; its window starts before START
  });

  it("a hole wholly before the boundary is irrelevant", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.loss("A", -300, { endMs: -200 });
    log.capture("A", 500);
    log.capture("B", 3000);
    assert.equal(ev(run, log).run.verification, "verified");
  });

  it("a hole inside the debounce window of the last crossing is harmless; one reaching its end is not", () => {
    const inside = lapsLog();
    const run = start(inside, "laps", LAPS);
    inside.capture("A", 1000);
    inside.loss("A", 1100, { endMs: 1299 });
    inside.capture("A", 5000);
    inside.capture("A", 9000);
    let r = ev(run, inside);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.crossingTicks.length, 3);

    const edge = lapsLog();
    const run2 = start(edge, "laps", LAPS);
    edge.capture("A", 1000);
    edge.loss("A", 1100, { endMs: 1300 }); // an edge at 1300 would have been a crossing
    edge.capture("A", 5000);
    r = ev(run2, edge);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.totalValid, false);
    assert.deepEqual(r.run.crossingTicks, [tick(1000)]);
  });

  it("an unknown-time loss spans the neighbouring records and waits for the upper one", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    log.capture("A", 1000);
    log.loss("A", 0, { unknown: true });
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending"); // bound not known yet
    assert.equal(r.run.closed, false);
    log.capture("A", 1100); // within 300 ms of the crossing at 1000: harmless, and itself a bounce
    log.capture("A", 5000);
    log.capture("A", 9000);
    r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.deepEqual(r.run.crossingTicks, [tick(1000), tick(5000), tick(9000)]);

    const wide = lapsLog();
    const run2 = start(wide, "laps", LAPS);
    wide.capture("A", 1000);
    wide.loss("A", 0, { unknown: true });
    wide.capture("A", 2000);
    r = ev(run2, wide);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /unknown time/);
    assert.equal(r.run.fault.reasons[0].lo, tick(1000));
    assert.equal(r.run.fault.reasons[0].hi, tick(2000));
  });

  it("a hole decides the run only once every sensor of its role has reported through it", () => {
    // laps: A loses edges after C2; C, the other start sensor, is still behind
    const laps = makeLog();
    laps.checkpoint("A", -100);
    laps.checkpoint("C", -100);
    const run = start(laps, "laps", [{ node_id: "A", role: "start" }, { node_id: "C", role: "start" }], { lapTarget: 2 });
    laps.capture("A", 1000);
    laps.capture("A", 5000);
    laps.capture("A", 9000);
    laps.loss("A", 9500, { endMs: 9600 });
    let r = ev(run, laps);
    assert.equal(r.run.verification, "pending");
    assert.equal(r.run.closed, false);
    laps.checkpoint("C", 10000);
    r = ev(r.run, laps);
    assert.equal(r.run.verification, "verified");
    assert.deepEqual(r.run.crossingTicks, [tick(1000), tick(5000), tick(9000)]);

    // sprint: A's hole comes before any start crossing; C reports one before it later
    const sprint = makeLog();
    sprint.checkpoint("A", -100);
    sprint.checkpoint("C", -100);
    sprint.checkpoint("B", -100);
    const run2 = start(sprint, "sprint", [{ node_id: "A", role: "start" }, { node_id: "C", role: "start" }, { node_id: "B", role: "finish" }]);
    sprint.loss("A", 200, { endMs: 700 });
    sprint.capture("B", 3000);
    r = ev(run2, sprint);
    assert.equal(r.run.verification, "pending");
    assert.equal(r.run.closed, false);
    sprint.capture("C", 100);
    sprint.checkpoint("C", 3000);
    r = ev(r.run, sprint);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.startTick, tick(100));
    assert.equal(r.run.finishTick, tick(3000));
  });

  it("one silent sensor keeps its role unconfirmed", () => {
    const log = makeLog();
    log.checkpoint("A", -100);
    log.checkpoint("C", -100);
    log.checkpoint("B", -100);
    const mappings = [...SPRINT, { node_id: "C", role: "start" }];
    const run = start(log, "sprint", mappings);
    log.capture("A", 1000);
    log.capture("B", 4000);
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending");
    assert.equal(r.crossings.find((c) => c.role === "start").confirmed, false);
    log.checkpoint("C", 1500);
    r = ev(run, log);
    assert.equal(r.run.verification, "verified");
  });

  it("a start-sensor reboot after S1 does not matter; before S1 it does", () => {
    const after = sprintLog();
    const run = start(after, "sprint", SPRINT);
    after.capture("A", 1000);
    after.checkpoint("A", 2000, { sensor_boot_id: 101, capture_seq: 0, end_seq: 0 });
    after.capture("B", 4000);
    assert.equal(ev(run, after).run.verification, "verified");

    const before = sprintLog();
    const run2 = start(before, "sprint", SPRINT);
    before.checkpoint("A", 500, { sensor_boot_id: 101, capture_seq: 0, end_seq: 0 });
    before.capture("B", 4000);
    const r = ev(run2, before);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /rebooted/);
  });

  it("a master timebase end keeps what was already decided", () => {
    const decided = sprintLog();
    const run = start(decided, "sprint", SPRINT);
    decided.capture("A", 1000);
    decided.capture("B", 1500);
    decided.masterEnd(2000);
    assert.equal(ev(run, decided).run.verification, "verified");

    const open = sprintLog();
    const run2 = start(open, "sprint", SPRINT);
    open.capture("A", 1000);
    open.masterEnd(2000);
    const r = ev(run2, open);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.fault.reasons[0].node_id, "0");
    assert.match(r.run.fault.reasons[0].reason, /timebase/);
  });

  it("a master reboot seen by the console ends an open run the same way", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    for (const ms of [1000, 6000, 11000]) log.capture("A", ms);
    const r = evaluateRun(endMasterSession(run), log.since(run.cursor));
    assert.equal(r.run.verification, "invalid");
    assert.deepEqual(r.run.crossingTicks, [tick(1000), tick(6000), tick(11000)]);
    assert.equal(r.run.totalValid, false);
    assert.match(r.run.fault.reasons[0].reason, /rebooted/);
  });

  it("capture times out of order are a fault of that sensor", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS, { debounceMs: 0 });
    log.capture("A", 1000);
    log.capture("A", 1000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /out of order/);
  });

  it("an unhealthy capture is an edge of unknown time", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    log.capture("A", 1000);
    log.capture("A", 3000, { flags: 5 });
    log.capture("A", 6000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /valid clock/);
  });

  it("a missing record stalls the sensor; a quarantined event in its place is a hole", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    log.capture("A", 1000);
    log.skip("A");
    log.capture("A", 5000);
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending");
    assert.deepEqual(r.run.crossingTicks, [tick(1000)]);

    const q = lapsLog();
    const run2 = start(q, "laps", LAPS);
    q.capture("A", 1000);
    q.skip("A");
    q.quarantine("A");
    q.capture("A", 5000);
    r = ev(run2, q);
    assert.equal(r.run.verification, "invalid");
    assert.match(r.run.fault.reasons[0].reason, /quarantined/);
  });

  it("rows from another master session are ignored", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 500);
    const b = log.capture("B", 1200);
    log.rows.push({ ...b, seq: log.rows.length + 1, master_tick: tick(900), end_tick: tick(900), master_boot_id: 2 }); // a stale copy under another session
    log.rows.splice(log.rows.indexOf(b), 1);
    let r = ev(run, log);
    assert.equal(r.run.verification, "pending");
    log.rows.push({ ...b, seq: log.rows.length + 1 });
    r = ev(run, log);
    assert.equal(secs(r.run.startTick, r.run.finishTick), 0.7);
  });

  it("collapses bounce edges per sensor inside the debounce window", () => {
    const log = sprintLog();
    const run = start(log, "sprint", SPRINT);
    log.capture("A", 1000);
    log.capture("A", 1065);
    log.capture("A", 1139);
    log.capture("B", 3000);
    log.capture("B", 3299);
    const r = ev(run, log);
    assert.equal(r.crossings.length, 2);
    assert.equal(secs(r.run.startTick, r.run.finishTick), 2);
    assert.equal(evaluateRun({ ...run, debounceMs: 0 }, log.since(run.cursor)).crossings.length, 5);
  });
});

describe("laps", () => {
  it("accumulates laps without a target; verified so far and still open", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    for (const ms of [1000, 16000, 31000, 46000]) log.capture("A", ms);
    const r = ev(run, log);
    assert.equal(r.laps.length, 3);
    assert.deepEqual(r.run.crossingTicks, [tick(1000), tick(16000), tick(31000), tick(46000)]);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, false);
    assert.equal(r.run.armed, true);
  });

  it("auto-stops at the lap target and ignores later crossings", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS, { lapTarget: 4 });
    for (const ms of [1000, 6000, 11500, 16500, 21000, 26000, 31000]) log.capture("A", ms);
    const r = ev(run, log);
    assert.deepEqual(r.run.crossingTicks, [1000, 6000, 11500, 16500, 21000].map(tick));
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.armed, false);
  });

  it("Stop fences at the stop tick and closes once the sensor is certain through it", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS);
    for (const ms of [1000, 11000, 21000]) log.capture("A", ms);
    const stopped = stopRun(run, tick(25000));
    assert.equal(stopped.armed, false);
    let r = ev(stopped, log);
    assert.equal(r.run.verification, "pending");
    assert.equal(r.run.closed, false);
    log.capture("A", 24000); // before Stop, delivered after: counts
    log.capture("A", 26000); // after Stop: ignored
    log.checkpoint("A", 27000);
    r = ev(stopped, log);
    assert.deepEqual(r.run.crossingTicks, [1000, 11000, 21000, 24000].map(tick));
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.closed, true);
  });

  it("a known-time loss just after Stop leaves the stopped run official", () => {
    const log = lapsLog();
    const run = stopRun(start(log, "laps", LAPS), tick(21500));
    for (const ms of [1000, 11000, 21000]) log.capture("A", ms);
    log.loss("A", 22000);
    log.checkpoint("A", 23000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "verified");
    assert.equal(r.run.fault, null);
  });

  it("a loss of unknown time just before Stop invalidates it; the laps before stay confirmed", () => {
    const log = lapsLog();
    const run = stopRun(start(log, "laps", LAPS), tick(21500));
    for (const ms of [1000, 11000, 21000]) log.capture("A", ms);
    log.loss("A", 0, { unknown: true });
    log.checkpoint("A", 23000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.totalValid, false);
    assert.deepEqual(r.run.crossingTicks, [1000, 11000, 21000].map(tick));
  });

  it("a significant hole before the target keeps only the laps before it", () => {
    const log = lapsLog();
    const run = start(log, "laps", LAPS, { lapTarget: 4 });
    for (const ms of [1000, 6000, 11000]) log.capture("A", ms);
    log.loss("A", 13000);
    log.capture("A", 16000);
    log.capture("A", 21000);
    const r = ev(run, log);
    assert.equal(r.run.verification, "invalid");
    assert.equal(r.run.closed, true);
    assert.equal(r.run.totalValid, false);
    assert.deepEqual(r.run.crossingTicks, [1000, 6000, 11000].map(tick));
  });

  it("Stop with one crossing is DNF, with none DNS", () => {
    const one = lapsLog();
    const run = stopRun(start(one, "laps", LAPS), tick(5000));
    one.capture("A", 1000);
    one.checkpoint("A", 6000);
    let r = ev(run, one);
    assert.equal(r.run.verification, "dnf");
    assert.equal(r.run.dnfReason, "DNF");
    const none = lapsLog();
    const run2 = stopRun(start(none, "laps", LAPS), tick(5000));
    none.checkpoint("A", 6000);
    r = ev(run2, none);
    assert.equal(r.run.dnfReason, "DNS");
  });
});

describe("run state helpers", () => {
  it("invalidateRun closes with the first fault", () => {
    const run = { mode: "laps", runId: "r", armed: true, closed: false, verification: "pending", fault: null, totalValid: true };
    const a = invalidateRun(run, [{ node_id: "0", reason: "x" }]);
    assert.equal(a.armed, false);
    assert.equal(a.closed, true);
    assert.equal(a.totalValid, false);
    assert.equal(a.fault.reasons[0].reason, "x");
    assert.equal(invalidateRun(a, [{ node_id: "0", reason: "y" }]).fault.reasons[0].reason, "x");
  });
  it("master boot change only matters for an open run", () => {
    const run = { armed: true, closed: false, masterBootId: 1 };
    assert.equal(shouldInvalidateOnMasterBoot(run, 2), true);
    assert.equal(shouldInvalidateOnMasterBoot(run, 1), false);
    assert.equal(shouldInvalidateOnMasterBoot({ ...run, armed: false }, 2), true);
    assert.equal(shouldInvalidateOnMasterBoot({ ...run, closed: true }, 2), false);
    assert.equal(shouldInvalidateOnMasterBoot(null, 2), false);
  });
});
