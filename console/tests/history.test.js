// History rows (recorded runs) and exports from raw ticks + frozen calibration (no IndexedDB: memory runs).
import { describe, it, beforeEach, vi } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";
import readExcelFile, { readSheet } from "read-excel-file/node";

vi.mock("../src/composables/useNotification", () => ({ useNotification: () => ({ success() {}, error() {}, open() {} }) }));

vi.mock("../src/lib/format.js", async (importOriginal) => ({
  ...await importOriginal(),
  downloadBlob: vi.fn(),
}));

const { useHistoryStore } = await import("../src/stores/history.js");
const { useTimingStore } = await import("../src/stores/timing.js");
const runLog = await import("../src/lib/runLog.js");
const { RUN_SCHEMA } = await import("../src/lib/engine.js");
const { freezeCalibration } = await import("../src/lib/calibration.js");
const { resultNs, lapsNs, frozenDurationNs } = await import("../src/lib/results.js");
const { formatDuration, downloadBlob } = await import("../src/lib/format.js");
const eventLog = await import("../src/lib/eventLog.js");
const { encodeRun, decodeRun } = await import("../src/lib/run-codec.js");
const { RADIO_PROTO_VERSION, USB_PROTO_VERSION } = await import("../src/lib/protocol.js");

const T0 = 1_000_000_000n;
const tick = (ms) => String(T0 + BigInt(ms) * 16000n);

// Read the actual workbook back in record order, including details immediately under each record.
function assertWorkbookRecords(sheetRows, records) {
  const rows = sheetRows.filter((row) => row.some((value) => value != null));
  let index = 0;
  const check = (expected) => {
    const row = rows[index++];
    assert.deepEqual(row.slice(0, expected.length), expected);
    assert.ok(row.slice(expected.length).every((v) => v == null));
  };
  for (const record of records) {
    const headers = ["Date", "Mode", "Note", "Result", "Status"];
    check(headers);
    check(headers.map((header) => record[header]));
    if (record["Lap Summary"]) {
      check(["Laps", "Best", "Average", "Total"]);
      check(Object.values(record["Lap Summary"]));
      check(["Lap", "Time", "Δ Before", "Δ Best"]);
      for (const lap of record["Lap Times"]) check(Object.values(lap));
    }
    if (record.Errors) {
      check(["Errors"]);
      record.Errors.forEach((error) => check([error]));
    }
  }
  assert.equal(index, rows.length, "no detached or extra data");
}

function run(fields) {
  return {
    schema: RUN_SCHEMA,
    radioProto: RADIO_PROTO_VERSION,
    usbProto: USB_PROTO_VERSION,
    runId: fields.runId,
    mode: fields.mode,
    note: "",
    boundaryTick: String(T0),
    masterBootId: 1,
    nodes: {},
    cursor: 0,
    lapTarget: null,
    debounceMs: 300,
    armed: false,
    closed: true,
    verification: "pending",
    classification: null,
    startTick: null,
    finishTick: null,
    crossingTicks: [],
    calibration: null,
    durationNs: null,
    fault: null,
    startedAt: 1,
    recordedAt: null,
    durable: true,
    ...fields,
  };
}

// A run past its first decided result.
const recorded = (fields) => run({ recordedAt: 1, ...fields });

// Frozen at the run's current ticks, as the timing store does.
function frozen(r) {
  const next = { ...r, calibration: freezeCalibration([], r.mode === "laps" ? r.crossingTicks : [r.startTick, r.finishTick].filter(Boolean), 1) };
  return { ...next, durationNs: frozenDurationNs(next) };
}

describe("history rows and exports", () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    eventLog._reset();
  });

  it("round-trips 64-bit sessions", async () => {
    const current = run({ runId: "64bit", mode: "laps",
      masterBootId: "18446744073709551615", nodes: { A: { boot: "9007199254740993", seq: 1, role: "start", cpSeq: 1 } }, closed: false });
    assert.deepEqual(decodeRun(encodeRun(current)), current);
    const history = useHistoryStore();
    await runLog.save({ ...current, verification: "verified", crossingTicks: [tick(0), tick(1000)], recordedAt: 1 });
    assert.equal(history.rows[0].masterBootId, current.masterBootId);
  });

  it("lists only runs past a decided result or with a classification", async () => {
    const history = useHistoryStore();
    for (const mode of ["sprint", "laps"]) {
      for (const state of [{ closed: false }, {},
        { startTick: tick(1000), crossingTicks: [tick(1000)] },
        { verification: "invalid" }]) {
        await runLog.save(run({ runId: `${mode}-${JSON.stringify(state)}`, mode, ...state }));
      }
    }
    assert.equal(history.rows.length, 0);
    assert.deepEqual(history.exportData(), { History: [] });
    await history.exportJson();
    const [blob] = downloadBlob.mock.calls.at(-1);
    assert.deepEqual(JSON.parse(await blob.text()).History, []);
  });

  it("manual classifications save untimed attempts and preserve measured results in exports", async () => {
    const history = useHistoryStore();
    const timing = useTimingStore();
    for (const classification of ["DNS", "DNF", "DSQ"]) {
      await runLog.save(run({ runId: classification, mode: "sprint", classification }));
      const row = history.rows.find((r) => r.runId === classification);
      assert.equal(history.status(row), classification);
      assert.equal(resultNs(row), null);
    }
    await runLog.save(recorded({ runId: "measured", mode: "laps", verification: "verified", crossingTicks: [tick(0), tick(1000), tick(2500)] }));
    assert.equal(await timing.classify("DSQ", { runId: "measured" }), true);
    const row = history.rows.find((r) => r.runId === "measured");
    assert.equal(row.verification, "verified");
    assert.equal(history.status(row), "DSQ");
    assert.equal(resultNs(row), 2_500_000_000n);
    assert.deepEqual(lapsNs(row), [1_000_000_000n, 1_500_000_000n]);
    assert.equal(history.exportData().History.find((r) => r.Result === "00:02.500").Status, "DSQ\nNot Saved");
    await history.exportJson();
    const exported = JSON.parse(await downloadBlob.mock.calls.at(-1)[0].text());
    assert.equal(exported.History.find((r) => r.Result === "00:02.500").Status, "DSQ\nNot Saved");
    assert.equal(await timing.classify("bad", { runId: "measured" }), false);
    assert.equal(await timing.classify("DNS", { runId: "missing" }), false);
    assert.equal(history.status(history.rows.find((r) => r.runId === "measured")), "DSQ");
  });

  it("clearing a classification retains measured times and removes only untimed classification rows", async () => {
    const history = useHistoryStore();
    const timing = useTimingStore();
    await runLog.save(recorded({ runId: "measured", mode: "laps", verification: "verified",
      classification: "DSQ", crossingTicks: [tick(0), tick(1000), tick(2500)] }));
    await timing.classify(null, { runId: "measured" });
    assert.equal(history.status(history.rows[0]), "verified");
    assert.deepEqual(lapsNs(history.rows[0]), [1_000_000_000n, 1_500_000_000n]);
    await runLog.save(recorded({ runId: "partial", mode: "laps", verification: "invalid",
      classification: "DNF", crossingTicks: [tick(0), tick(1000)] }));
    await timing.classify(null, { runId: "partial" });
    assert.deepEqual(lapsNs(history.rows.find((r) => r.runId === "partial")), [1_000_000_000n]);
    for (const classification of ["DNS", "DNF", "DSQ"]) {
      await runLog.save(run({ runId: classification, mode: "sprint", classification }));
      await timing.classify(null, { runId: classification });
      assert.ok(!history.rows.some((r) => r.runId === classification));
    }
  });

  it("Excel stays display-only while JSON includes all records and recovery data", async () => {
    const history = useHistoryStore();
    const note = '=1+1, 한글 "note"\nsecond line';
    await runLog.save(recorded({ runId: "sprint", mode: "sprint", note, verification: "verified",
      masterBootId: "18446744073709551615", startTick: "9007199254740993000", finishTick: "9007199254756993000",
      durationNs: "1000000000", classification: "DSQ" }));
    await runLog.save(recorded({ runId: "laps", mode: "laps", verification: "verified", note: "Car 7",
      crossingTicks: [tick(0), tick(2000), tick(3500), tick(6000), tick(8500)],
      fault: { kind: "test", reasons: [{ role: "finish", reason: "Sensor ABC: Capture Loss", lo: tick(100), hi: null }] } }));
    await runLog.save(run({ runId: "manual", mode: "sprint", classification: "DNS" }));
    // Same date and note must not merge two attempts or detach their laps.
    await runLog.save(recorded({ runId: "same-context", mode: "laps", note: "Car 7", verification: "verified",
      crossingTicks: [tick(0), tick(1000), tick(3000)] }));
    const snapshot = history.exportData();
    assert.deepEqual(snapshot.History.filter((r) => r.Mode === "Laps").map((r) => r["Lap Times"].map((lap) => lap.Time)),
      [["00:02.000", "00:01.500", "00:02.500", "00:02.500"], ["00:01.000", "00:02.000"]]);
    assert.deepEqual(Object.keys(snapshot), ["History"]);
    const lapRecord = snapshot.History.find((r) => r.Mode === "Laps");
    assert.deepEqual(Object.keys(snapshot.History[0]), ["Date", "Mode", "Note", "Result", "Status"]);
    assert.deepEqual(lapRecord["Lap Times"].map((r) => [r.Lap, r.Time, r["Δ Before"], r["Δ Best"]]), [
      [1, "00:02.000", "-", "+0.500"], [2, "00:01.500", "−0.500", "BEST"],
      [3, "00:02.500", "+1.000", "+1.000"], [4, "00:02.500", "0.000", "+1.000"],
    ]);
    assert.deepEqual(Object.values(lapRecord["Lap Summary"]), [4, "00:01.500 (#2)", "00:02.125", "00:08.500"]);
    assert.equal(snapshot.History.find((r) => r.Note === note).Result, "00:01.000");
    assert.equal(snapshot.History.find((r) => r.Mode === "Start → Finish" && r.Note === "-").Result, "-");
    assert.equal(lapRecord.Errors[0], "Finish\nSensor ABC: Capture Loss\nafter +00:00.100 (end not known yet)");
    await history.exportJson();
    const [jsonBlob, jsonName] = downloadBlob.mock.calls.at(-1);
    assert.match(jsonName, /\.json$/);
    const json = JSON.parse(await jsonBlob.text());
    assert.deepEqual(json.History, snapshot.History);
    assert.equal(json.version, 2);
    assert.equal(json.recovery.runs.length, 4);
    assert.equal(json.recovery.runs.find(r => r.runId === "sprint").masterBootId, "18446744073709551615");
    await history.exportExcel();
    const [excelBlob, filename] = downloadBlob.mock.calls.at(-1);
    assert.match(filename, /^fsk-timekeeper-all-.*\.xlsx$/);
    assert.equal(excelBlob.type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    const buffer = Buffer.from(await excelBlob.arrayBuffer());
    const workbook = await readExcelFile(buffer);
    assert.deepEqual(workbook.map((s) => s.sheet), ["History"], "one sheet for every record and all its laps");
    assertWorkbookRecords(workbook[0].data, json.History);
    await history.exportJson("sprint");
    const filtered = JSON.parse(await downloadBlob.mock.calls.at(-1)[0].text());
    assert.deepEqual(filtered.History, snapshot.History);
    assert.equal(filtered.recovery.runs.length, 4);
    await history.exportExcel("sprint");
    const [filteredExcel] = downloadBlob.mock.calls.at(-1);
    assertWorkbookRecords(await readSheet(Buffer.from(await filteredExcel.arrayBuffer()), "History"), history.exportData("sprint").History);
  });

  it("Stop keeps the recorded laps and total", async () => {
    const history = useHistoryStore();
    const crossings = [tick(1000), tick(6000), tick(11500)];
    const r = recorded({ runId: "laps-1", mode: "laps", verification: "verified", crossingTicks: crossings, closed: false });
    await runLog.save(r);
    await runLog.save(frozen({ ...r, closed: true }));
    const row = history.rows[0];
    assert.equal(history.rows.length, 1);
    assert.equal(row.durationNs, "10500000000");
    assert.equal(resultNs(row), 10_500_000_000n);
    const record = history.exportData().History[0];
    assert.equal(record.Status, "Verified\nNot Saved");
    assert.equal(record.Result, "00:10.500");
    assert.deepEqual(Object.values(record["Lap Summary"]), [2, "00:05.000 (#1)", "00:05.250", "00:10.500"]);
  });

  it("a later loss keeps the confirmed laps but voids the total, also after the freeze", async () => {
    const history = useHistoryStore();
    const crossings = [tick(1000), tick(6000), tick(11500)];
    const r = recorded({ runId: "laps-1", mode: "laps", verification: "verified", crossingTicks: crossings.slice(0, 2), closed: false });
    await runLog.save(r);
    const fault = { kind: "measurement", reasons: [{ role: "start", reason: "Sensor ABC: Capture Loss", lo: tick(12000), hi: null }] };
    await runLog.save(frozen({ ...r, closed: true, verification: "invalid", crossingTicks: crossings, fault }));
    const row = history.rows[0];
    assert.equal(history.rows.length, 1);
    assert.equal(row.durationNs, null);
    assert.equal(resultNs(row), null);
    assert.deepEqual(lapsNs(row), [5_000_000_000n, 5_500_000_000n]);
    const record = history.exportData().History[0];
    assert.equal(record.Status, "Invalid\nNot Saved");
    assert.equal(record.Result, "-");
    assert.deepEqual(Object.values(record["Lap Summary"]), [2, "00:05.000 (#1)", "00:05.250", "-"]);
    assert.match(record.Errors[0], /^Start\nSensor ABC: Capture Loss\n/);
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
