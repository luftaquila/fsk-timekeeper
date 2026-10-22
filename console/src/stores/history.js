// Results history: the recorded runs of the run log (raw ticks, frozen calibration, exact ns).
import writeExcelFile from "write-excel-file/universal";
import { defineStore } from "pinia";
import { ref, computed } from "vue";
import * as eventLog from "../lib/eventLog";
import { enqueue, flush } from "../lib/storageQueue";
import * as runLog from "../lib/runLog";
import * as backup from "../lib/backup";
import { useTimingStore } from "./timing";
import { isRecord, isCarRecord } from "../lib/engine";
import { ROLE_LABEL } from "../lib/constants";
import { carRun } from "../lib/results";
import { downloadBlob, timestampSlug, formatDuration } from "../lib/format";
import { historyRecord, lapSummary, lapDelta, statusValue } from "../lib/historyPresentation";
import { faultWindow, formatFaultReason } from "../lib/fault-text";
import { useDeviceStore } from "./device";

// History rows of a run list, newest first.
const recordsOf = (runs) => runs.filter(isRecord).sort((a, b) => b.startedAt - a.startedAt);

export const useHistoryStore = defineStore("history", () => {
  const importing = ref(false);
  const clearing = ref(false);
  const revision = ref(0); // bumped on every run-log change
  runLog.onChange(() => revision.value++);
  const rows = computed(() => {
    revision.value;
    return recordsOf(runLog.list());
  });

  function removeRow(runId) {
    return runLog.remove(runId);
  }

  async function clearAll() {
    const timing = useTimingStore(), device = useDeviceStore();
    if (clearing.value || importing.value || timing.armed || timing.starting || timing.classifying || device.connecting || device.updateInProgress || device.dfuInProgress) {
      throw new Error("Stop measurement and wait for current operations before clearing all data.");
    }
    clearing.value = true;
    try {
      await device.pauseData();
      await eventLog.flush();
      await flush();
      await enqueue(() => eventLog.clearAllData());
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

  // History rows of a run: a camera heat lists its cars that are records.
  function entries(row) {
    if (row.mode !== "camera-laps") return [{ row, view: row }];
    return Object.keys(row.results).filter((car) => isCarRecord(row.results[car])).map((car) => ({ row, view: carRun(row, car), car }));
  }

  // Provisional calibration source for a row not frozen yet, an open run (same master boot).
  function edgesFor(row) {
    const device = useDeviceStore();
    return device.ppsBootId === row.masterBootId ? device.ppsEdges : [];
  }

  const status = statusValue;

  // Display columns shared by Excel and the human-readable section of recovery JSON.
  function exportData(mode = null, source = rows.value) {
    const listed = source.filter(row => !mode || row.mode === mode).flatMap(entries);
    return { History: listed.map(({ row: run, view: row, car }) => {
      const edges = edgesFor(run);
      const record = historyRecord(row, edges);
      if (car) {
        record.Mode = historyRecord(run, edges).Mode;
        record.Note = run.note ? `#${car}\n${run.note}` : `#${car}`; // as History's note cell shows it
      }
      const stats = lapSummary(row, edges);
      if (stats) {
        // An unassigned crossing may be the car's: its lap shows ?, the count may be one more, and
        // the best lap and average are unknown.
        const unknown = stats.best == null;
        record["Lap Summary"] = { Laps: unknown ? `${stats.laps.length}+?` : stats.laps.length,
          Best: unknown ? "?" : `${formatDuration(stats.best)} (#${stats.bestIndex + 1})`,
          Average: unknown ? "?" : formatDuration(stats.sum, BigInt(stats.laps.length)), Total: formatDuration(stats.total) };
        record["Lap Times"] = stats.laps.map((ns, i) => ({ Lap: i + 1,
          Time: stats.held.has(i) ? "?" : formatDuration(ns),
          "Δ Before": i > 0 && (stats.held.has(i) || stats.held.has(i - 1)) ? "?" : lapDelta(i > 0 ? ns - stats.laps[i - 1] : null),
          "Δ Best": unknown || stats.held.has(i) ? "?" : ns === stats.best ? "BEST" : lapDelta(ns - stats.best) }));
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
    const headers = ["Date", "Mode", "Note", "Result", "Status", "Calibration"];
    const widths = [28, 26, 40, 20, 24, 14];
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
        data.push([{ value: "Errors", fontWeight: "bold", backgroundColor: "#E9EDF2", columnSpan: 6 }, null, null, null, null, null]);
        for (const error of record.Errors) data.push([{ value: error, columnSpan: 6, wrap: true,
          height: Math.max(24, error.split("\n").reduce((sum, line) => sum + Math.max(1, Math.ceil(line.length / 130)), 0) * 16 + 8) }, null, null, null, null, null]);
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
    await flush();
    await eventLog.flush();
    const recovery = await backup.snapshot(useDeviceStore().ppsEdges);
    return backup.pack(exportData(null, recordsOf(recovery.runs)).History, recovery);
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
      await flush();
      const result = await backup.merge(data);
      timing.markRestored();
      await device.loadLog();
      return result;
    } finally { importing.value = false; }
  }

  return { rows, importing, clearing, flush, recoveryJson, importJson, removeRow, clearAll, filtered, entries, status, edgesFor, exportData, exportExcel, exportJson };
});
