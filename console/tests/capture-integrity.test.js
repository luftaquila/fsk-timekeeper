import { test } from "vitest";
import assert from "node:assert/strict";
import { verifyCaptures } from "../src/lib/capture-integrity.js";

const run = {
  boundaryTick: "100",
  masterBootId: 1,
  nodes: {
    a: { boot: 2, seq: 0, role: "start" },
    b: { boot: 3, seq: 0, role: "finish" },
  },
};
const capture = (node, seq, tick, extra = {}) => ({
  node_id: node,
  sensor_boot_id: run.nodes[node].boot,
  master_boot_id: 1,
  capture_seq: seq,
  master_tick: String(tick),
  end_seq: seq,
  end_tick: String(tick),
  flags: 15,
  sync_age_ms: 0,
  ...extra,
});
const checkpoint = (node, seq, tick) => capture(node, seq, tick, { flags: 47 });

test("delayed and reordered captures verify only after every source confirms its prefix", () => {
  const rows = [capture("b", 1, 200), checkpoint("b", 1, 250), capture("a", 1, 150)];
  assert.equal(verifyCaptures(run, rows).events.length, 0);
  const verified = verifyCaptures(run, [...rows, checkpoint("a", 1, 250)]);
  assert.deepEqual(verified.events.map((row) => row.master_tick), ["150", "200"]);
  assert.equal(verified.fault, null);
});

test("a missing middle crossing stays pending until it is delivered", () => {
  const rows = [capture("a", 2, 180), checkpoint("a", 2, 250), checkpoint("b", 0, 250)];
  assert.equal(verifyCaptures(run, rows).events.length, 0);
  assert.equal(verifyCaptures(run, rows).fault, null);
  assert.equal(verifyCaptures(run, [...rows, capture("a", 1, 150)]).events.length, 2);
});

test("loss affects its capture interval, not the batch or current health", () => {
  const rows = [capture("a", 1, 150), capture("b", 1, 200), capture("a", 2, 210, { flags: 31 }), checkpoint("a", 2, 250), checkpoint("b", 1, 250)];
  const result = verifyCaptures(run, rows);
  assert.deepEqual(result.events.map((row) => row.master_tick), ["150", "200"]);
  assert.equal(result.fault.node_id, "a");
  assert.deepEqual(verifyCaptures(run, rows.reverse()), result);
});

test("a loss before the new start does not poison the new run", () => {
  const rows = [capture("a", 1, 90, { flags: 31 }), capture("a", 2, 150), capture("b", 1, 200), checkpoint("a", 2, 250), checkpoint("b", 1, 250)];
  const result = verifyCaptures(run, rows);
  assert.equal(result.fault, null);
  assert.equal(result.events.length, 2);
});

test("a new sensor boot cannot bridge a running measurement", () => {
  const rows = [checkpoint("a", 0, 250), { ...checkpoint("b", 0, 250), sensor_boot_id: 4 }];
  assert.equal(verifyCaptures(run, rows).fault.node_id, "b");
});

test("loss notification waits for a checkpoint and cannot invalidate a delivered capture", () => {
  const loss = capture("a", 1, 150, { flags: 31 });
  assert.equal(verifyCaptures(run, [loss]).fault, null);
  const rows = [loss, capture("a", 1, 150), checkpoint("a", 1, 250), checkpoint("b", 0, 250)];
  assert.equal(verifyCaptures(run, rows).fault, null);
  assert.equal(verifyCaptures(run, rows).events.length, 1);
});

test("an overlapping loss range still detects its missing tail", () => {
  const rows = [capture("a", 1, 150), capture("a", 1, 150, { flags: 31, end_seq: 2, end_tick: "180" }), checkpoint("a", 2, 250), checkpoint("b", 0, 250)];
  assert.equal(verifyCaptures(run, rows).fault?.node_id, "a");
});

test("source sequence cannot move backwards in capture time", () => {
  const rows = [capture("a", 1, 180), capture("a", 2, 150), checkpoint("a", 2, 250), checkpoint("b", 0, 250)];
  assert.equal(verifyCaptures(run, rows).fault?.node_id, "a");
});

test("an acknowledged prefix survives a later fault with unknown capture time", () => {
  const rows = [
    capture("a", 1, 150),
    capture("b", 1, 200),
    checkpoint("a", 1, 220),
    checkpoint("b", 1, 220),
    capture("a", 2, 0, { flags: 80 }),
    checkpoint("a", 2, 300),
    checkpoint("b", 1, 300),
  ];
  assert.deepEqual(verifyCaptures(run, rows).events.map((row) => row.master_tick), ["150", "200"]);
});

test("delivered ranges still validate capture clock order", () => {
  const rows = [
    capture("a", 1, 180),
    capture("a", 1, 150, { flags: 80, end_seq: 2, end_tick: "180" }),
    capture("a", 2, 150),
    checkpoint("a", 2, 250),
    checkpoint("b", 0, 250),
  ];
  assert.equal(verifyCaptures(run, rows).fault?.node_id, "a");
});

test("a later loss cannot close the run before another source delivers its earlier finish", () => {
  const rows = [capture("a", 1, 150), capture("a", 2, 210, { flags: 31 }), checkpoint("a", 2, 250)];
  assert.equal(verifyCaptures(run, rows).fault, null);
  const verified = verifyCaptures(run, [...rows, capture("b", 1, 200), checkpoint("b", 1, 250)]);
  assert.deepEqual(verified.events.map((row) => row.master_tick), ["150", "200"]);
  assert.equal(verified.fault.node_id, "a");
});

for (const reversedTick of [90, 100]) {
  test(`capture reversal to ${reversedTick} cannot escape validation at the START boundary`, () => {
    const rows = [capture("a", 1, 150), capture("a", 2, reversedTick), capture("b", 1, 200), checkpoint("a", 2, 250), checkpoint("b", 1, 250)];
    const result = verifyCaptures(run, rows);
    assert.equal(result.fault?.node_id, "a");
    assert.equal(result.events.length, 0);
  });
}

test("a reversal wholly before START does not invalidate subsequent healthy captures", () => {
  const rows = [capture("a", 1, 80), capture("a", 2, 70), capture("a", 3, 150), capture("b", 1, 200), checkpoint("a", 3, 250), checkpoint("b", 1, 250)];
  const result = verifyCaptures(run, rows);
  assert.equal(result.fault, null);
  assert.deepEqual(result.events.map((row) => row.master_tick), ["150", "200"]);
});

test("a master clock fault at or after the boundary ends the session", () => {
  const rows = [
    capture("a", 1, 150),
    { ...capture("a", 1, 160), node_id: "0", sensor_boot_id: 1, flags: 16, capture_seq: 0, end_seq: 0 },
    checkpoint("a", 1, 250),
    checkpoint("b", 0, 250),
  ];
  const result = verifyCaptures(run, rows);
  assert.equal(result.fault?.node_id, "0");
  // The capture before the fault tick stays; nothing at or after it is accepted.
  assert.deepEqual(result.events.map((row) => row.master_tick), ["150"]);
  assert.equal(verifyCaptures(run, [...rows, capture("b", 1, 200), checkpoint("b", 1, 260)]).events.length, 1);
});
