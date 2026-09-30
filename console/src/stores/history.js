/* Results history (IndexedDB `results`, memory fallback) + CSV/JSON export.
 * Rows keep raw ticks, the frozen calibration and exact ns; rows from before schema 3
 * (`result`/`laps` in ms) are shown as stored. */
import { defineStore } from "pinia";
import { ref, toRaw } from "vue";
import * as eventLog from "../lib/eventLog";
import { getAll, put, remove, clear } from "../lib/idb";
import { MODE_LABEL } from "../lib/constants";
import { toCsv, downloadBlob, timestampSlug, formatDuration } from "../lib/format";
import { isOldFormatRow, resultNs, lapsNs, calibrationMethod } from "../lib/results";
import { useDeviceStore } from "./device";

// Run fields copied into its history row.
const RUN_FIELDS = [
  "mode",
  "verification",
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

  function db() {
    return eventLog.getDb();
  }

  async function init() {
    try {
      if (db()) {
        const all = await getAll(db(), eventLog.STORE_RESULTS);
        rows.value = all.sort((a, b) => b.createdAt - a.createdAt);
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

  // Called at START: creates the pending row.
  async function open(run, { masterDevId = null } = {}) {
    const row = {
      runId: run.runId,
      note: run.note || "",
      ...fieldsOf(run),
      masterDevId,
      cursorSeq: run.cursor,
      throughSeq: null,
      createdAt: run.startedAt || Date.now(),
      updatedAt: Date.now(),
    };
    return save(row);
  }

  // Called whenever the engine changes a run.
  async function upsert(run, { throughSeq = null } = {}) {
    const existing = rows.value.find((r) => r.runId === run.runId);
    if (!existing) return null;
    // A copy of the raw object: a reactive proxy cannot be structured-cloned into IndexedDB.
    const row = { ...toRaw(existing) };
    if (isOldFormatRow(row)) {
      // Rows of another run format keep their data; only the closing state changes.
      row.verification = run.verification;
      row.fault = run.fault ? { kind: run.fault.kind, reasons: plain(run.fault.reasons) } : row.fault ?? null;
    } else {
      Object.assign(row, fieldsOf(run));
      row.note = run.note || row.note;
    }
    if (throughSeq != null) row.throughSeq = throughSeq;
    row.updatedAt = Date.now();
    return save(row);
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

  function status(row) {
    return row.verification === "dnf" ? row.dnfReason || "DNF" : row.verification;
  }

  function csvRow(r) {
    const old = isOldFormatRow(r);
    const edges = edgesFor(r);
    const result = resultNs(r, edges);
    const laps = r.mode === "laps" ? lapsNs(r, edges) : [];
    return [
      r.id,
      new Date(r.createdAt).toISOString(),
      r.startedUtc ?? "",
      MODE_LABEL[r.mode] || r.mode,
      r.note,
      r.verification,
      r.verification === "dnf" ? r.dnfReason || "" : "",
      result != null ? formatDuration(result) : "",
      !old && result != null ? String(result) : "",
      r.startTick ?? "",
      r.finishTick ?? "",
      (r.crossingTicks || []).join(" "),
      laps.map((ns) => formatDuration(ns)).join(" "),
      old ? "" : laps.map(String).join(" "),
      r.lapTarget ?? "",
      r.debounceMs ?? "",
      old ? (r.ppb != null ? `ppb ${r.ppb}` : "") : calibrationMethod(r, edges) ?? "",
      r.masterBootId ?? "",
      r.boundaryTick ?? "",
      r.durable === false ? "no" : "yes",
    ];
  }

  function csvText(mode = null) {
    const headers = [
      "id",
      "date",
      "started_utc",
      "mode",
      "note",
      "status",
      "dnf_reason",
      "result",
      "duration_ns",
      "start_tick",
      "finish_tick",
      "crossing_ticks",
      "laps",
      "lap_ns",
      "lap_target",
      "debounce_ms",
      "calibration",
      "master_boot_id",
      "boundary_tick",
      "durable",
    ];
    return toCsv(headers, filtered(mode).map(csvRow));
  }

  function exportCsv(mode = null) {
    const blob = new Blob([csvText(mode)], { type: "text/csv;charset=utf-8" });
    downloadBlob(blob, `fsk-timekeeper-${mode || "all"}-${timestampSlug()}.csv`);
  }

  async function exportJson(mode = null, { withEvidence = false } = {}) {
    const results = filtered(mode);
    const out = { exported_at: new Date().toISOString(), results };
    if (withEvidence) {
      out.evidence = {};
      for (const r of results) {
        if (r.cursorSeq == null) continue;
        const to = r.throughSeq ?? eventLog.getLastSeq();
        out.evidence[r.runId] = await eventLog.rowsBetween(r.cursorSeq, to);
      }
    }
    const blob = new Blob([JSON.stringify(out, null, 2)], { type: "application/json" });
    downloadBlob(blob, `fsk-timekeeper-${mode || "all"}-${timestampSlug()}.json`);
  }

  return { rows, ready, init, open, upsert, removeRow, clearAll, filtered, status, edgesFor, csvText, exportCsv, exportJson };
});
