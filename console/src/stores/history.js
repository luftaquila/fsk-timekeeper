/* Results history (IndexedDB `results`, memory fallback) + Excel/JSON export.
 * Rows keep raw ticks, the frozen calibration and exact ns; rows of another run schema are not shown. */
import writeExcelFile from "write-excel-file/universal";
import { defineStore } from "pinia";
import { ref, toRaw } from "vue";
import * as eventLog from "../lib/eventLog";
import { getAll, put, remove, transaction, done } from "../lib/idb";
import { enqueue, flush } from "../lib/storageQueue";
import * as runLog from "../lib/runLog";
import * as backup from "../lib/backup";
import { useTimingStore } from "./timing";
import { RUN_SCHEMA } from "../lib/engine";
import { ROLE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { downloadBlob, timestampSlug, formatDuration } from "../lib/format";
import { resultNs, resultTicks, lapsNs } from "../lib/results";
import { historyRecord, lapSummary, lapDelta, statusValue } from "../lib/historyPresentation";
import { faultWindow, formatFaultReason } from "../lib/fault-text";
import { useDeviceStore } from "./device";

// Run fields copied into its history row.
const RUN_FIELDS = [
  "mode",
  "verification",
  "classification",
  "closed",
  "startTick",
  "finishTick",
  "crossingTicks",
  "totalValid",
  "calibration",
  "durationNs",
  "lapTarget",
  "debounceMs",
  "startedUtc",
  "gpsAtStart",
  "masterBootId",
  "boundaryTick",
  "schema",
  "radioProto",
  "usbProto",
];

function plain(value) {
  return value == null ? value : JSON.parse(JSON.stringify(toRaw(value)));
}

export const useHistoryStore = defineStore("history", () => {
  const importing = ref(false);
  const clearing = ref(false);
  const rows = ref([]); // newest first
  let memoryId = 0;


  function db() {
    return eventLog.getDb();
  }

  async function init() {
    try {
      if (db()) {
        const all = await getAll(db(), eventLog.STORE_RESULTS);
        rows.value = all.filter((row) => row.schema === RUN_SCHEMA).sort((a, b) => b.createdAt - a.createdAt);
      }
    } catch (error) {
      eventLog.reportWriteFailure(error);
      rows.value = [];
    }
  }

  async function save(row) {
    if (db()) {
      try {
        row.id = await put(db(), eventLog.STORE_RESULTS, row);
      } catch (error) {
        if (row.id == null) row.id = -++memoryId;
        row.durable = false;
        eventLog.reportWriteFailure(error);
      }
    } else {
      if (row.id == null) row.id = -++memoryId;
      row.durable = false;
    }
    const idx = rows.value.findIndex((r) => r.runId === row.runId);
    if (idx >= 0) rows.value[idx] = row;
    else rows.value.unshift(row);
    return row.id;
  }

  function fieldsOf(run) {
    const out = {};
    for (const key of RUN_FIELDS) out[key] = plain(run[key] ?? null);
    out.fault = run.fault ? { kind: run.fault.kind, reasons: plain(run.fault.reasons) } : null;
    out.durable = run.durable !== false;
    return out;
  }

  // Runs and capture evidence are stored separately. Results begin only once a measured
  // interval is verified or the operator assigns a classification; later updates use that same row.
  function hasResult(run) {
    return run.verification === "verified" && resultNs(run) != null;
  }

  async function saveResult(run, { throughSeq = null } = {}) {
    if (runLog.isDeleted(run.runId)) return null;
    const existing = rows.value.find((r) => r.runId === run.runId);
    const measured = hasResult(run);
    if (!existing && ((!measured && !RESULT_CLASSIFICATIONS.includes(run.classification)) || run.historyId != null)) return null;
    const row = existing ? { ...toRaw(existing) } : {
      runId: run.runId,
      note: run.note || "",
      masterDevId: run.masterDevId ?? null,
      cursorSeq: run.cursor,
      throughSeq: null,
      createdAt: run.startedAt || Date.now(),
    };
    if (measured || !existing) {
      const frozen = row.calibration && JSON.stringify(resultTicks(row)) === JSON.stringify(resultTicks(run))
        ? { calibration: row.calibration, durationNs: row.durationNs } : null;
      Object.assign(row, fieldsOf(run));
      if (frozen) Object.assign(row, frozen);
      row.note = run.note || row.note;
      if (throughSeq != null) row.throughSeq = throughSeq;
    } else {
      // Stopping or a later fault cannot overwrite an already recorded result.
      row.closed = run.closed || row.closed;
      if (run.durable === false) row.durable = false;
      const ticks = resultTicks(row);
      if (run.calibration && ticks.length && ticks.every((tick) => run.calibration.points?.[tick])) {
        row.calibration = plain(run.calibration);
        row.durationNs = null;
        const ns = resultNs(row);
        row.durationNs = ns == null ? null : String(ns);
      }
    }
    if (run.classification === null && RESULT_CLASSIFICATIONS.includes(existing?.classification)) {
      return saveClassification(row, null);
    }
    if (RESULT_CLASSIFICATIONS.includes(run.classification)) row.classification = run.classification;
    row.updatedAt = Date.now();
    return save(row);
  }

  function upsert(run, options = {}) {
    // Capture the state now and serialize writes: several laps or calibration can arrive
    // while IndexedDB is still inserting the first result.
    const snapshot = plain(run);
    return enqueue(() => saveResult(snapshot, options));
  }

  // Finalization changes timing only. Read the latest row inside the mutation queue
  // so an operator's classification (including Clear) always survives a late freeze.
  function updateCalibration(run) {
    const snapshot = plain(run);
    return enqueue(async () => {
      const existing = rows.value.find(r => r.runId === snapshot.runId);
      if (!existing || existing.calibration || runLog.isDeleted(snapshot.runId)) return;
      const ticks = resultTicks(existing);
      if (!ticks.length || !ticks.every(tick => snapshot.calibration?.points?.[tick])) return;
      const row = { ...plain(existing), calibration: snapshot.calibration, durationNs: null, updatedAt: Date.now() };
      const ns = resultNs(row);
      row.durationNs = ns == null ? null : String(ns);
      if (snapshot.durable === false) row.durable = false;
      return save(row);
    });
  }

  async function saveClassification(row, classification) {
    if (classification === null && resultNs(row) == null && !lapsNs(row).length) {
      if (db() && row.id > 0) await remove(db(), eventLog.STORE_RESULTS, row.id);
      rows.value = rows.value.filter((r) => r.runId !== row.runId);
      return null;
    }
    return save({ ...plain(row), classification, closed: true, updatedAt: Date.now() });
  }

  function classify(runId, classification) {
    if (classification !== null && !RESULT_CLASSIFICATIONS.includes(classification)) return Promise.reject(new Error("Invalid classification"));
    return enqueue(() => {
      const row = rows.value.find((r) => r.runId === runId);
      if (!row) throw new Error("Record not found");
      return saveClassification(row, classification);
    });

  }

  function deleteRows(select) {
    return enqueue(async () => {
      const removed = select(rows.value);
      const ids = new Set(removed.map(r => r.runId));
      if (db()) {
        const tx = transaction(db(), [eventLog.STORE_RESULTS, runLog.STORE_RUNS], "readwrite");
        const finished = done(tx);
        for (const row of removed) {
          if (row.id > 0) tx.objectStore(eventLog.STORE_RESULTS).delete(row.id);
          tx.objectStore(runLog.STORE_RUNS).put({ runId: row.runId, deleted: true });
        }
        await finished;
      }
      for (const id of ids) runLog.tombstone(id);
      rows.value = rows.value.filter(r => !ids.has(r.runId));
    });
  }

  function removeRow(id) { return deleteRows(rows => rows.filter(r => r.id === id)); }
  async function clearAll() {
    const timing = useTimingStore(), device = useDeviceStore();
    if (clearing.value || importing.value || timing.armed || timing.starting || timing.classifying || device.connecting || device.updateInProgress || device.dfuInProgress) {
      throw new Error("Stop measurement and wait for current operations before clearing all data.");
    }
    clearing.value = true;
    try {
      await device.pauseData();
      await timing.settleResults();
      await eventLog.flush();
      await flush();
      await enqueue(() => eventLog.clearAllData());
      rows.value = [];
      memoryId = 0;
      timing.discardRecords();
      device.discardRecords();
    } finally {
      device.clearingData = false;
      clearing.value = false;
    }
  }

  function filtered(mode = null) {
    return mode ? rows.value.filter((r) => r.mode === mode) : rows.value.slice();
  }

  // Provisional calibration source for a row still waiting for its freeze (same master boot).
  function edgesFor(row) {
    const device = useDeviceStore();
    return device.ppsBootId === row.masterBootId ? device.ppsEdges : [];
  }

  const status = statusValue;

  // Display columns shared by Excel and the human-readable section of recovery JSON.
  function exportData(mode = null, source = rows.value) {
    return { History: source.filter(row => !mode || row.mode === mode).map((row) => {
      const edges = edgesFor(row);
      const record = historyRecord(row, edges);
      const stats = lapSummary(row, edges);
      if (stats) {
        record["Lap Summary"] = { Laps: stats.laps.length,
          Best: `${formatDuration(stats.best)} (#${stats.bestIndex + 1})`,
          Average: formatDuration(stats.total, BigInt(stats.laps.length)), Total: formatDuration(stats.total) };
        record["Lap Times"] = stats.laps.map((ns, i) => ({ Lap: i + 1,
          Time: formatDuration(ns), "Δ Before": lapDelta(i > 0 ? ns - stats.laps[i - 1] : null),
          "Δ Best": ns === stats.best ? "BEST" : lapDelta(ns - stats.best) }));
      }
      if (row.fault?.reasons?.length) {
        record.Errors = row.fault.reasons.map((reason) => [
          reason.role ? ROLE_LABEL[reason.role] || reason.role : "",
          formatFaultReason(reason.reason), faultWindow(reason, row.boundaryTick),
        ].filter(Boolean).join("\n"));
      }
      return record;
    }) };
  }

  async function exportExcel(mode = null) {
    await useTimingStore().settleResults();
    const headers = ["Date", "Mode", "Note", "Result", "Status"];
    const widths = [28, 26, 40, 20, 24];
    const data = [];
    const addTable = (columns, records, primary = false) => {
      data.push(columns.map((value) => ({ value, fontWeight: "bold", height: 24,
        backgroundColor: primary ? "#334155" : "#E9EDF2", textColor: primary ? "#FFFFFF" : "#1F2937" })));
      for (const record of records) {
        const values = columns.map((header) => record[header]);
        const lines = Math.max(...values.map((value, i) => String(value).split("\n")
          .reduce((sum, line) => sum + Math.max(1, Math.ceil(line.length / (widths[i] - 2))), 0)));
        data.push(values.map((value) => ({ value, wrap: true, height: Math.max(24, lines * 16 + 8), alignVertical: "center" })));
      }
    };
    for (const record of exportData(mode).History) {
      if (data.length) data.push([]);
      addTable(headers, [record], true);
      if (record["Lap Summary"]) addTable(Object.keys(record["Lap Summary"]), [record["Lap Summary"]]);
      if (record["Lap Times"]) addTable(["Lap", "Time", "Δ Before", "Δ Best"], record["Lap Times"]);
      if (record.Errors) {
        data.push([{ value: "Errors", fontWeight: "bold", backgroundColor: "#E9EDF2", columnSpan: 5 }, null, null, null, null]);
        for (const error of record.Errors) data.push([{ value: error, columnSpan: 5, wrap: true,
          height: Math.max(24, error.split("\n").reduce((sum, line) => sum + Math.max(1, Math.ceil(line.length / 130)), 0) * 16 + 8) }, null, null, null, null]);
      }
    }
    if (!data.length) addTable(headers, [], true);
    const blob = await writeExcelFile(data, {
      sheet: "History", columns: widths.map((width) => ({ width })),
    }).toBlob();
    downloadBlob(blob, `fsk-timekeeper-${mode || "all"}-${timestampSlug()}.xlsx`);
  }

  async function recoveryJson() {
    if (clearing.value) throw new Error("Wait for data deletion to finish before backing up.");
    await useTimingStore().settleResults();
    await eventLog.flush();
    const recovery = await backup.snapshot(rows.value, useDeviceStore().ppsEdges);
    recovery.results.sort((a, b) => b.createdAt - a.createdAt);
    return backup.pack(exportData(null, recovery.results).History, recovery);
  }

  async function exportJson() {
    const blob = new Blob([await recoveryJson()], { type: "application/json" });
    downloadBlob(blob, `fsk-timekeeper-all-${timestampSlug()}.json`);
  }

  async function importJson(file) {
    const timing = useTimingStore(), device = useDeviceStore();
    if (clearing.value || importing.value || timing.armed || timing.starting || timing.classifying || device.updateInProgress) {
      throw new Error("Stop measurement and wait for current operations before restoring.");
    }
    importing.value = true;
    try {
      if (file.size > backup.MAX_BACKUP_BYTES) throw new Error("JSON exceeds the 64 MB limit.");
      const data = await backup.unpack(await file.text());
      await timing.settleResults();
      const result = await backup.merge(data);
      await init();
      timing.markRestored();
      await device.loadLog();
      return result;
    } finally { importing.value = false; }
  }

  return { rows, importing, clearing, init, flush, recoveryJson, importJson, upsert, updateCalibration, classify, removeRow, clearAll, filtered, status, edgesFor, exportData, exportExcel, exportJson };
});
