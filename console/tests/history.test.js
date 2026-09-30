// History rows and CSV from raw ticks + frozen calibration (no IndexedDB: memory rows).
import { describe, it, beforeEach, vi } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";

vi.mock("../src/composables/useNotification", () => ({ useNotification: () => ({ success() {}, error() {}, open() {} }) }));

const { useHistoryStore } = await import("../src/stores/history.js");
const { freezeCalibration } = await import("../src/lib/calibration.js");
const { resultNs, lapsNs } = await import("../src/lib/results.js");
const { formatDuration } = await import("../src/lib/format.js");
const eventLog = await import("../src/lib/eventLog.js");

const T0 = 1_000_000_000n;
const tick = (ms) => String(T0 + BigInt(ms) * 16000n);
const csvRows = (text) => text.trim().split("\r\n").map((l) => l.split(","));

function run(fields) {
  return {
    schema: 3,
    radioProto: 11,
    usbProto: 2,
    runId: fields.runId,
    mode: fields.mode,
    note: "",
    boundaryTick: String(T0),
    masterBootId: 1,
    cursor: 0,
    lapTarget: null,
    debounceMs: 300,
    verification: "pending",
    dnfReason: null,
    startTick: null,
    finishTick: null,
    crossingTicks: [],
    totalValid: true,
    calibration: null,
    durationNs: null,
    fault: null,
    startedAt: 1,
    durable: true,
    ...fields,
  };
}

describe("history rows and CSV", () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    eventLog._reset();
  });

  it("an invalid laps run lists its confirmed laps and no total", async () => {
    const history = useHistoryStore();
    const crossings = [tick(1000), tick(6000), tick(11500)];
    const r = run({ runId: "laps-1", mode: "laps", verification: "invalid", crossingTicks: crossings, totalValid: false, calibration: freezeCalibration([], crossings, 1) });
    await history.open(r);
    await history.upsert(r);
    const row = history.rows[0];
    assert.equal(row.durable, false); // memory only
    assert.equal(resultNs(row), null);
    assert.deepEqual(lapsNs(row), [5_000_000_000n, 5_500_000_000n]);
    const [header, line] = csvRows(history.csvText());
    const col = (name) => line[header.indexOf(name)];
    assert.equal(col("status"), "invalid");
    assert.equal(col("result"), "");
    assert.equal(col("duration_ns"), "");
    assert.equal(col("laps"), "00:05.000 00:05.500");
    assert.equal(col("lap_ns"), "5000000000 5500000000");
    assert.equal(col("crossing_ticks"), crossings.join(" "));
    assert.equal(col("calibration"), "nominal");
    assert.equal(col("durable"), "no");
  });

  it("a DNF row, a verified sprint and a pre-v3 row side by side", async () => {
    const history = useHistoryStore();
    const s = run({ runId: "s", mode: "sprint", verification: "verified", startTick: tick(10), finishTick: tick(4010) });
    s.calibration = freezeCalibration([], [s.startTick, s.finishTick], 1);
    s.durationNs = "4000000000";
    await history.open(s);
    await history.open(run({ runId: "d", mode: "sprint", verification: "dnf", dnfReason: "DNF", startTick: tick(10) }));
    history.rows.push({ id: 9, runId: "old", mode: "laps", note: "", result: 12345.4, laps: [6000, 6345], verification: "verified", createdAt: 0, ppb: 12 });
    const [header, ...lines] = csvRows(history.csvText());
    const byStatus = Object.fromEntries(lines.map((l) => [l[header.indexOf("status")] + (l[header.indexOf("dnf_reason")] || ""), l]));
    assert.equal(byStatus.verified.length, header.length);
    const sprint = lines.find((l) => l[header.indexOf("status")] === "verified" && l[header.indexOf("mode")] !== "Laps");
    assert.equal(sprint[header.indexOf("start_tick")], s.startTick);
    assert.equal(sprint[header.indexOf("result")], "00:04.000");
    assert.equal(sprint[header.indexOf("duration_ns")], "4000000000");
    assert.equal(byStatus.dnfDNF[header.indexOf("result")], "");
    const old = lines.find((l) => l[0] === "9");
    assert.equal(old[header.indexOf("result")], "00:12.345");
    assert.equal(old[header.indexOf("duration_ns")], "");
    assert.equal(old[header.indexOf("laps")], "00:06.000 00:06.345");
    assert.equal(history.status(history.rows.find((r) => r.runId === "d")), "DNF");
  });

  it("the list and the result come from the same points", () => {
    const crossings = [tick(0), String(BigInt(tick(1000)) + 8000n), tick(2001)]; // a lap ends on a half millisecond
    const calibration = freezeCalibration([], crossings, 1);
    const r = run({ runId: "c", mode: "laps", verification: "verified", crossingTicks: crossings, calibration });
    const laps = lapsNs(r);
    assert.equal(resultNs(r), laps.reduce((a, b) => a + b, 0n));
    assert.equal(formatDuration(resultNs(r)), "00:02.001");
  });
});
