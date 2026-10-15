/* Results history (IndexedDB `results`, memory fallback) + Excel/JSON export.
 * Rows keep raw ticks, the frozen calibration and exact ns; rows from before schema 3
 * (`result`/`laps` in ms) are shown as stored. */
import writeExcelFile from "write-excel-file/universal";
import { defineStore } from "pinia";
import { ref, toRaw } from "vue";
import * as eventLog from "../lib/eventLog";
import { getAll, put, remove, clear } from "../lib/idb";
import { ROLE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { downloadBlob, timestampSlug, formatDuration } from "../lib/format";
import { isOldFormatRow, resultNs, resultTicks, lapsNs } from "../lib/results";
import { historyRecord, lapSummary, lapDelta, statusValue } from "../lib/historyPresentation";
import { faultWindow, formatFaultReason } from "../lib/fault-text";
import { useDeviceStore } from "./device";

// Run fields copied into its history row.
const RUN_FIELDS = [
  "mode",
  "verification",
  "classification",
  "closed",
  "dnfReason",
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
  const rows = ref([]); // newest first
  const ready = ref(false);
  let memoryId = 0;
  let pendingWrite = Promise.resolve();

  function db() {
    return eventLog.getDb();
  }

  async function init() {
    try {
      if (db()) {
        const all = await getAll(db(), eventLog.STORE_RESULTS);
        // Old versions created a result row on Start, before any measured result existed.
        const empty = all.filter((row) => !isOldFormatRow(row) && row.verification === "pending" && !RESULT_CLASSIFICATIONS.includes(row.classification) && resultNs(row) == null);
        for (const row of empty) await remove(db(), eventLog.STORE_RESULTS, row.id);
        const emptyIds = new Set(empty.map((row) => row.id));
        rows.value = all.filter((row) => !emptyIds.has(row.id)).sort((a, b) => b.createdAt - a.createdAt);
      }
    } catch {
      rows.value = [];
    }
    ready.value = true;
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
    return !isOldFormatRow(run) && run.verification === "verified" && resultNs(run) != null;
  }

  async function saveResult(run, { throughSeq = null } = {}) {
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
    if ((measured || !existing) && !isOldFormatRow(run)) {
      Object.assign(row, fieldsOf(run));
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
    const write = pendingWrite.then(() => saveResult(snapshot, options));
    pendingWrite = write.catch(() => {});
    return write;
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
    const write = pendingWrite.then(() => {
      const row = rows.value.find((r) => r.runId === runId);
      if (!row) throw new Error("Record not found");
      return saveClassification(row, classification);
    });
    pendingWrite = write.catch(() => {});
    return write;
  }

  async function removeRow(id) {
    rows.value = rows.value.filter((r) => r.id !== id);
    if (db() && id > 0) {
      try {
        await remove(db(), eventLog.STORE_RESULTS, id);
      } catch {
        /* ignore */
      }
    }
  }

  async function clearAll() {
    rows.value = [];
    if (db()) {
      try {
        await clear(db(), eventLog.STORE_RESULTS);
      } catch {
        /* ignore */
      }
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

  // A single display-only snapshot feeds both file formats. Raw storage fields never leave here.
  function exportData(mode = null) {
    return { History: filtered(mode).map((row) => {
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

  async function exportJson(mode = null) {
    const blob = new Blob([JSON.stringify(exportData(mode), null, 2)], { type: "application/json" });
    downloadBlob(blob, `fsk-timekeeper-${mode || "all"}-${timestampSlug()}.json`);
  }

  return { rows, ready, init, upsert, classify, removeRow, clearAll, filtered, status, edgesFor, exportData, exportExcel, exportJson };
});
