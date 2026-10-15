// History rows and exports from raw ticks + frozen calibration (no IndexedDB: memory rows).
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
const { freezeCalibration } = await import("../src/lib/calibration.js");
const { resultNs, lapsNs } = await import("../src/lib/results.js");
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
    schema: 3,
    radioProto: 12,
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

describe("history rows and exports", () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    eventLog._reset();
  });

  it("round-trips 64-bit sessions and preserves finished results from the previous protocol", async () => {
    const current = run({ runId: "64bit", mode: "laps", radioProto: RADIO_PROTO_VERSION, usbProto: USB_PROTO_VERSION,
      masterBootId: "18446744073709551615", nodes: { A: { boot: "9007199254740993", seq: 1, role: "start", cpSeq: 1 } }, closed: false });
    assert.deepEqual(decodeRun(encodeRun(current)).run, current);
    const old = run({ runId: "legacy", mode: "sprint", closed: true, armed: true, verification: "verified",
      startTick: tick(10), finishTick: tick(4010), durationNs: "4000000000" });
    const restored = decodeRun(encodeRun(old));
    assert.equal(restored.protocolChanged, false);
    assert.equal(restored.run.armed, false);
    assert.equal(restored.run.verification, "verified");
    assert.equal(restored.run.durationNs, old.durationNs);
    const history = useHistoryStore();
    await history.upsert({ ...current, verification: "verified", crossingTicks: [tick(0), tick(1000)] });
    assert.equal(history.rows[0].masterBootId, current.masterBootId);
  });

  it("Start, incomplete Stop and faults before a measured result create no records", async () => {
    const history = useHistoryStore();
    for (const mode of ["sprint", "laps"]) {
      for (const state of [{}, { closed: true },
        { closed: true, startTick: tick(1000), crossingTicks: [tick(1000)] },
        { closed: true, verification: "invalid", totalValid: false }]) {
        await history.upsert(run({ runId: `${mode}-${JSON.stringify(state)}`, mode, ...state }));
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
    for (const classification of ["DNS", "DNF", "DSQ"]) {
      await history.upsert(run({ runId: classification, mode: "sprint", classification, closed: true }));
      const row = history.rows.find((r) => r.runId === classification);
      assert.equal(history.status(row), classification);
      assert.equal(resultNs(row), null);
    }
    const measured = run({ runId: "measured", mode: "laps", verification: "verified", crossingTicks: [tick(0), tick(1000), tick(2500)] });
    await history.upsert(measured);
    await history.classify("measured", "DSQ");
    const row = history.rows.find((r) => r.runId === "measured");
    assert.equal(row.verification, "verified");
    assert.equal(history.status(row), "DSQ");
    assert.equal(resultNs(row), 2_500_000_000n);
    assert.deepEqual(lapsNs(row), [1_000_000_000n, 1_500_000_000n]);
    assert.equal(history.exportData().History.find((r) => r.Result === "00:02.500").Status, "DSQ\nNot Saved");
    await history.exportJson();
    const exported = JSON.parse(await downloadBlob.mock.calls.at(-1)[0].text());
    assert.equal(exported.History.find((r) => r.Result === "00:02.500").Status, "DSQ\nNot Saved");
    await assert.rejects(history.classify("measured", "bad"), /Invalid classification/);
    await assert.rejects(history.classify("missing", "DNS"), /Record not found/);
  });

  it("clearing a classification retains measured times and removes only untimed classification rows", async () => {
    const history = useHistoryStore();
    const measured = run({ runId: "measured", mode: "laps", verification: "verified",
      classification: "DSQ", crossingTicks: [tick(0), tick(1000), tick(2500)] });
    await history.upsert(measured);
    const id = history.rows[0].id;
    await history.classify("measured", null);
    assert.equal(history.status(history.rows[0]), "verified");
    assert.equal(history.rows[0].id, id);
    assert.deepEqual(lapsNs(history.rows[0]), [1_000_000_000n, 1_500_000_000n]);
    // The current-run upsert path must also clear the saved classification.
    await history.upsert(measured);
    await history.upsert({ ...measured, classification: null });
    assert.equal(history.status(history.rows[0]), "verified");
    await history.upsert(run({ runId: "partial", mode: "laps", verification: "invalid",
      classification: "DNF", totalValid: false, crossingTicks: [tick(0), tick(1000)] }));
    await history.classify("partial", null);
    assert.deepEqual(lapsNs(history.rows.find((r) => r.runId === "partial")), [1_000_000_000n]);
    for (const classification of ["DNS", "DNF", "DSQ"]) {
      await history.upsert(run({ runId: classification, mode: "sprint", classification }));
      await history.classify(classification, null);
      assert.ok(!history.rows.some((r) => r.runId === classification));
    }
  });

  it("Excel and JSON contain exactly the same displayed values and no storage fields", async () => {
    const history = useHistoryStore();
    const note = '=1+1, 한글 "note"\nsecond line';
    await history.upsert(run({ runId: "sprint", mode: "sprint", note, verification: "verified",
      masterBootId: "18446744073709551615", startTick: "9007199254740993000", finishTick: "9007199254756993000",
      durationNs: "1000000000", classification: "DSQ" }));
    await history.upsert(run({ runId: "laps", mode: "laps", verification: "verified", note: "Car 7",
      crossingTicks: [tick(0), tick(2000), tick(3500), tick(6000), tick(8500)],
      fault: { kind: "test", reasons: [{ role: "finish", reason: "Sensor ABC lost captures.", lo: tick(100), hi: null }] } }));
    await history.upsert(run({ runId: "manual", mode: "sprint", classification: "DNS" }));
    // Same date and note must not merge two attempts or detach their laps.
    history.rows.push({ id: 100, runId: "same-context", createdAt: 1, mode: "laps", note: "Car 7",
      verification: "verified", result: 3000, laps: [1000, 2000] });
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
    const evidence = vi.spyOn(eventLog, "rowsBetween");
    try {
      await history.exportJson();
      const [jsonBlob, jsonName] = downloadBlob.mock.calls.at(-1);
      assert.match(jsonName, /\.json$/);
      const json = JSON.parse(await jsonBlob.text());
      assert.deepEqual(json, snapshot);
      assert.equal(evidence.mock.calls.length, 0);
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
      assert.deepEqual(Object.keys(filtered), ["History"]);
      assert.equal(filtered.History.length, 2);
      await history.exportExcel("sprint");
      const [filteredExcel] = downloadBlob.mock.calls.at(-1);
      assertWorkbookRecords(await readSheet(Buffer.from(await filteredExcel.arrayBuffer()), "History"), filtered.History);
    } finally {
      evidence.mockRestore();
    }
  });

  it("later Stop or fault preserves the last recorded laps and total", async () => {
    const history = useHistoryStore();
    const crossings = [tick(1000), tick(6000), tick(11500)];
    const r = run({ runId: "laps-1", mode: "laps", verification: "verified", crossingTicks: crossings });
    await history.upsert(r);
    const id = history.rows[0].id;
    await history.upsert({ ...r, closed: true, verification: "invalid", totalValid: false,
      calibration: freezeCalibration([], crossings, 1) });
    const row = history.rows[0];
    assert.equal(history.rows.length, 1);
    assert.equal(row.id, id);
    assert.equal(row.verification, "verified");
    assert.equal(resultNs(row), 10_500_000_000n);
    assert.deepEqual(lapsNs(row), [5_000_000_000n, 5_500_000_000n]);
    assert.equal(history.exportData().History[0].Status, "Verified\nNot Saved");
    assert.equal(history.exportData().History[0].Result, "00:10.500");
    assert.deepEqual(history.exportData().History[0]["Lap Times"].map((r) => r.Time), ["00:05.000", "00:05.500"]);
  });

  it("concurrent first-lap and later-lap writes create one result row", async () => {
    const history = useHistoryStore();
    const r = run({ runId: "burst", mode: "laps", verification: "verified", crossingTicks: [tick(0), tick(1000)] });
    await Promise.all([
      history.upsert(r),
      history.upsert({ ...r, crossingTicks: [tick(0), tick(1000), tick(2000)] }),
      history.upsert({ ...r, crossingTicks: [tick(0), tick(1000), tick(2000)], closed: true }),
    ]);
    assert.equal(history.rows.length, 1);
    assert.equal(history.rows[0].closed, true);
    assert.equal(resultNs(history.rows[0]), 2_000_000_000n);
  });

  it("a DNF row, a verified sprint and a pre-v3 row side by side", async () => {
    const history = useHistoryStore();
    const s = run({ runId: "s", mode: "sprint", verification: "verified", startTick: tick(10), finishTick: tick(4010) });
    s.calibration = freezeCalibration([], [s.startTick, s.finishTick], 1);
    s.durationNs = "4000000000";
    await history.upsert(s);
    // Previously saved classifications remain readable.
    history.rows.push({ ...run({ runId: "d", mode: "sprint", verification: "dnf", dnfReason: "DNF", startTick: tick(10) }), id: 8, createdAt: 0 });
    history.rows.push({ id: 9, runId: "old", mode: "laps", note: "", result: 12345.4, laps: [6000, 6345], verification: "verified", createdAt: 0, ppb: 12 });
    const exported = history.exportData();
    const sprint = exported.History.find((r) => r.Result === "00:04.000");
    assert.equal(sprint.Status, "Verified\nNot Saved");
    assert.equal(exported.History.find((r) => r.Status === "DNF").Result, "-");
    assert.equal(exported.History.find((r) => r.Mode === "Laps").Result, "00:12.345");
    assert.deepEqual(exported.History.find((r) => r.Mode === "Laps")["Lap Times"].map((r) => r.Time), ["00:06.000", "00:06.345"]);
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
