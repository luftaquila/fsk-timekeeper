/* Results history (IndexedDB `results` store, memory fallback) + CSV/JSON export. */
import { defineStore } from "pinia";
import { ref, toRaw } from "vue";
import * as eventLog from "../lib/eventLog";
import { getAll, put, remove, clear } from "../lib/idb";
import { MODE_LABEL } from "../lib/constants";
import { formatLapMs, masterTickDurationsMs } from "../lib/event-timing";
import { toCsv, downloadBlob, timestampSlug } from "../lib/format";

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

  function lapsMs(run) {
    const ppb = run.calib?.ppb ?? 0;
    return (run.lapTicks || []).map((t) => masterTickDurationsMs([t], ppb));
  }

  async function save(row) {
    if (db()) {
      try {
        row.id = await put(db(), eventLog.STORE_RESULTS, row);
      } catch {
        if (row.id == null) row.id = -++memoryId;
      }
    } else if (row.id == null) row.id = -++memoryId;
    const idx = rows.value.findIndex((r) => r.runId === row.runId);
    if (idx >= 0) rows.value[idx] = row;
    else rows.value.unshift(row);
    return row.id;
  }

  // Called at START: creates the pending row.
  async function open(run, { masterDevId = null } = {}) {
    const row = {
      runId: run.runId,
      mode: run.mode,
      note: run.note || "",
      result: null,
      laps: [],
      lapTarget: run.lapTarget ?? null,
      ppb: run.calib?.ppb ?? null,
      gps: run.calib ? { fix: run.calib.fix, sats: run.calib.sats, span: run.calib.span } : null,
      startedUtc: run.startedUtc ?? null,
      verification: run.verification,
      boundaryTick: run.boundaryTick,
      masterBootId: run.masterBootId,
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
    // Copy the raw object: a reactive proxy (nested `gps`) cannot be structured-cloned into IndexedDB.
    const row = { ...toRaw(existing) };
    row.note = run.note || row.note;
    row.result = run.result;
    row.laps = lapsMs(run);
    row.verification = run.verification;
    row.fault = run.fault ? { kind: run.fault.kind, reasons: run.fault.reasons } : null;
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

  function exportCsv(mode = null) {
    const headers = ["id", "date", "started_utc", "mode", "note", "result_ms", "result", "laps_ms", "lap_target", "hfxo_ppb", "verification", "master_boot_id", "boundary_tick"];
    const data = filtered(mode).map((r) => [
      r.id,
      new Date(r.createdAt).toISOString(),
      r.startedUtc ?? "",
      MODE_LABEL[r.mode] || r.mode,
      r.note,
      r.result ?? "",
      r.result != null ? formatLapMs(r.result) : "",
      (r.laps || []).join(" "),
      r.lapTarget ?? "",
      r.ppb ?? "",
      r.verification,
      r.masterBootId,
      r.boundaryTick,
    ]);
    const blob = new Blob([toCsv(headers, data)], { type: "text/csv;charset=utf-8" });
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

  return { rows, ready, init, open, upsert, removeRow, clearAll, filtered, exportCsv, exportJson };
});
