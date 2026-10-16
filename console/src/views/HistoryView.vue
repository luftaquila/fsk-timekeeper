<script setup>
import { ref, computed, nextTick } from "vue";
import { useTimingStore } from "../stores/timing";
import { useDeviceStore } from "../stores/device";
import { useHistoryStore } from "../stores/history";
import { useNotification } from "../composables/useNotification";
import { MODES, MODE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { formatDuration, fmtDateTime } from "../lib/format";
import { historyRecord, lapSummary, lapDelta, statusLabel } from "../lib/historyPresentation";

import FaultReasons from "../components/FaultReasons.vue";

const history = useHistoryStore();
const notyf = useNotification();
const timing = useTimingStore();
const device = useDeviceStore();
const importInput = ref(null);
const importBlocked = computed(() => exporting.value || history.importing || timing.armed || timing.starting || timing.classifying || device.updateInProgress);
const classifying = ref(false);
const exporting = ref(false);
const filter = ref("");
const lapDialog = ref(null);
const lapPopup = ref(null);

const rows = computed(() => history.filtered(filter.value || null));

function result(r) {
  return historyRecord(r, history.edgesFor(r)).Result;
}
function lapStats(r) {
  return lapSummary(r, history.edgesFor(r));
}
const lapSummaries = computed(() => new Map(rows.value.map((r) => [r.runId, lapStats(r)])));

async function openLaps(r) {
  const stats = lapSummaries.value.get(r.runId);
  if (!stats) return;
  lapPopup.value = { ...stats, note: r.note, date: fmtDateTime(r.createdAt) };
  await nextTick();
  lapDialog.value.showModal();
}
function closeLaps() {
  lapDialog.value.close();
}
function badgeClass(r) {
  const status = history.status(r);
  return status === "verified" ? "badge-success" : ["invalid", "DSQ"].includes(status) ? "badge-danger" : status === "DNS" ? "badge-default" : "badge-warning";
}
async function classify(r, event) {
  classifying.value = true;
  try {
    const classification = event.target.value || null;
    if (timing.run?.runId === r.runId) await timing.classify(classification);
    else await history.classify(r.runId, classification);
  } catch (error) {
    notyf.error(error.message || "Could not save classification");
  } finally {
    event.target.value = history.status(history.rows.find((row) => row.runId === r.runId) || r);
    classifying.value = false;
  }
}
async function exportFile(format) {
  if (exporting.value) return;
  exporting.value = true;
  try {
    if (format === "excel") await history.exportExcel(filter.value || null);
    else await history.exportJson();
  } catch (error) {
    notyf.error(error.message || "Export failed");
  } finally {
    exporting.value = false;
  }
}
async function importFile(event) {
  const file = event.target.files?.[0];
  event.target.value = "";
  if (!file || importBlocked.value) return;
  try {
    const { added, skipped, runs } = await history.importJson(file);
    notyf.success(`Imported ${added} records and ${runs} run snapshots; ${skipped} duplicates skipped`);
  } catch (error) { notyf.error(error.message || "Import failed"); }
}

async function remove(r) {
  try { await history.removeRow(r.id); notyf.success("Deleted"); }
  catch (error) { notyf.error(error.message || "Delete failed"); }
}
async function clearAll() {
  if (!window.confirm("Delete all history records?")) return;
  try { await history.clearAll(); notyf.success("History cleared"); }
  catch (error) { notyf.error(error.message || "Delete failed"); }
}
</script>

<template>
  <div class="history">
    <div class="toolbar">
      <select v-model="filter" class="form-select">
        <option value="">All modes</option>
        <option v-for="m in MODES" :key="m" :value="m">{{ MODE_LABEL[m] }}</option>
      </select>
      <span class="count muted">{{ rows.length }} run{{ rows.length === 1 ? "" : "s" }}</span>
      <div class="spacer"></div>
      <button class="btn btn-ghost btn-sm" :disabled="!rows.length || exporting || history.importing" @click="exportFile('excel')">Export Excel</button>
      <button class="btn btn-ghost btn-sm" :disabled="exporting || history.importing" @click="exportFile('json')">Export JSON</button>
      <input ref="importInput" type="file" accept=".json,application/json" hidden @change="importFile" />
      <button class="btn btn-ghost btn-sm" :disabled="importBlocked" @click="importInput.click()">Import JSON</button>
      <button class="btn btn-ghost btn-sm" :disabled="!history.rows.length || history.importing" @click="clearAll">Clear All</button>
    </div>

    <div class="card">
      <div v-if="!rows.length" class="empty-state">No runs recorded yet</div>
      <div v-else class="table-scroll">
        <table class="data-table">
          <thead>
            <tr>
              <th>Date</th>
              <th class="centered">Mode</th>
              <th>Note</th>
              <th class="centered">Result</th>
              <th class="centered">Status</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <template v-for="r in rows" :key="r.id">
              <tr>
                <td class="mono nowrap">{{ fmtDateTime(r.createdAt) }}</td>
                <td class="centered"><span class="badge mode-chip" :class="{ 'mode-sprint': r.mode === 'sprint', 'mode-laps': r.mode === 'laps' }">{{ MODE_LABEL[r.mode] || r.mode }}</span></td>
                <td class="note-cell">{{ r.note || "-" }}</td>
                <td class="mono nowrap result-cell centered">
                  <button v-if="lapSummaries.get(r.runId)" class="result-link" aria-haspopup="dialog" @click="openLaps(r)">{{ result(r) }}</button>
                  <span v-else>{{ result(r) }}</span>
                </td>
                <td class="centered">
                  <select class="badge status-select" :class="badgeClass(r)" :value="history.status(r)"
                    aria-label="Record status" :disabled="classifying || timing.starting || timing.classifying || history.importing" @change="classify(r, $event)">
                    <option v-if="!RESULT_CLASSIFICATIONS.includes(history.status(r))" :value="history.status(r)" disabled>{{ statusLabel(r) }}</option>
                    <option v-if="r.classification" value="">Clear</option>
                    <option v-for="value in RESULT_CLASSIFICATIONS" :key="value" :value="value">{{ value }}</option>
                  </select>
                  <span v-if="r.durable === false" class="badge badge-danger">Not Saved</span>
                </td>
                <td class="actions"><button class="btn btn-ghost btn-sm" :disabled="history.importing" @click="remove(r)">Delete</button></td>
              </tr>
              <tr v-if="r.fault?.reasons?.length" class="fault-row"><td colspan="6"><FaultReasons :reasons="r.fault.reasons" :boundary-tick="r.boundaryTick" /></td></tr>
            </template>
          </tbody>
        </table>
      </div>
    </div>
    <dialog ref="lapDialog" class="lap-dialog" aria-labelledby="lap-dialog-title" @click.self="closeLaps" @close="lapPopup = null">
      <div v-if="lapPopup" class="lap-dialog-content">
        <div class="lap-dialog-header">
          <div>
            <h3 id="lap-dialog-title">Lap Times</h3>
            <p class="lap-context">{{ lapPopup.date }}<span v-if="lapPopup.note"> · {{ lapPopup.note }}</span></p>
          </div>
          <button class="btn btn-ghost" aria-label="Close lap details" autofocus @click="closeLaps">✕</button>
        </div>
        <dl class="lap-stats">
          <div><dt>Laps</dt><dd class="mono">{{ lapPopup.laps.length }}</dd></div>
          <div><dt>Best</dt><dd class="mono">{{ formatDuration(lapPopup.best) }} <span class="best-index">(#{{ lapPopup.bestIndex + 1 }})</span></dd></div>
          <div><dt>Average</dt><dd class="mono">{{ formatDuration(lapPopup.total, BigInt(lapPopup.laps.length)) }}</dd></div>
          <div><dt>Total</dt><dd class="mono">{{ formatDuration(lapPopup.total) }}</dd></div>
        </dl>
        <div class="lap-table-scroll">
          <table class="lap-list">
            <thead><tr><th>Lap</th><th>Time</th><th>Δ Before</th><th>Δ Best</th></tr></thead>
            <tbody>
              <tr v-for="(ns, i) in lapPopup.laps" :key="i" :class="{ best: ns === lapPopup.best }">
                <td>{{ i + 1 }}</td>
                <td class="mono">{{ formatDuration(ns) }}</td>
                <td class="mono" :class="{ slower: i > 0 && ns > lapPopup.laps[i - 1] }">{{ lapDelta(i > 0 ? ns - lapPopup.laps[i - 1] : null) }}</td>
                <td class="mono">{{ ns === lapPopup.best ? "BEST" : lapDelta(ns - lapPopup.best) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </dialog>
  </div>
</template>

<style scoped>
.data-table th,
.data-table td {
  padding: 0.9rem 1rem;
  font-size: 1rem;
  line-height: 1.5;
  vertical-align: middle;
}
.data-table th {
  background: var(--bg-hover);
  color: var(--text-primary);
  font-size: 1rem;
  font-weight: 700;
}
.data-table td { color: var(--text-primary); }
.data-table .centered { text-align: center; }
.data-table .result-cell { font-size: 1.125rem; }
.data-table .badge {
  padding: 0.3rem 0.65rem;
  font-size: 1rem;
  font-weight: 600;
  white-space: nowrap;
}
.status-select { border: 0; cursor: pointer; font-family: inherit; text-align: center; }
.status-select option { background: var(--bg-card); color: var(--text-primary); }
.mode-chip {
  background: var(--bg-hover);
  color: var(--text-primary);
  border: 0;
}
.mode-chip.mode-sprint {
  background: color-mix(in srgb, var(--accent-primary) 14%, var(--bg-card));
  color: color-mix(in srgb, var(--accent-primary) 65%, var(--text-primary));
}
.mode-chip.mode-laps {
  background: color-mix(in srgb, var(--accent-secondary) 14%, var(--bg-card));
  color: color-mix(in srgb, var(--accent-secondary) 65%, var(--text-primary));
}
.note-cell { max-width: 24rem; overflow-wrap: anywhere; }
.data-table .actions .btn { font-size: 1rem; }
.data-table .actions .btn:hover {
  color: var(--accent-danger);
  border-color: var(--accent-danger);
  background: color-mix(in srgb, var(--accent-danger) 12%, var(--bg-card));
}
.fault-row td { white-space: normal; max-width: 0; color: var(--accent-danger); }
.fault-row :deep(.fault-reasons) { max-width: calc(100vw - 6rem); }
.history {
  display: flex;
  flex-direction: column;
  gap: 1rem;
}
.toolbar {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  flex-wrap: wrap;
}
.toolbar .btn,
.toolbar .form-select {
  height: 2.25rem;
  padding: 0 0.75rem;
  font-size: 1rem;
  font-weight: 500;
  line-height: 1.25rem;
}
.toolbar .form-select {
  border: 0;
  width: auto;
}
.spacer {
  flex: 1;
}
.muted {
  color: var(--text-tertiary);
  font-size: 1rem;
}
.nowrap {
  white-space: nowrap;
}
.result-link {
  padding: 0;
  border: 0;
  background: none;
  color: var(--accent-primary);
  font: inherit;
  text-align: left;
  cursor: pointer;
}
.result-link:hover { text-decoration: underline; }
.result-link:focus-visible { outline: 2px solid var(--accent-primary); outline-offset: 4px; }
.lap-dialog {
  margin: auto;
  padding: 0;
  width: min(36rem, calc(100vw - 2rem));
  max-height: calc(100dvh - 2rem);
  border: 1px solid var(--border-color);
  border-radius: 12px;
  background: var(--bg-card);
  color: var(--text-primary);
  box-shadow: var(--shadow-hover);
  overflow: auto;
}
.lap-dialog::backdrop { background: rgb(0 0 0 / 55%); }
.lap-dialog-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  padding: 1rem 1.25rem;
  background: var(--bg-hover);
  border-bottom: 1px solid var(--border-color);
}
.lap-dialog-header h3 { font-size: 1.15rem; }
.lap-dialog-header button { flex-shrink: 0; }
.lap-context { margin-top: 0.35rem; color: var(--text-secondary); overflow-wrap: anywhere; }
.lap-stats { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 1rem; padding: 1.25rem; }
.lap-stats dt { margin-bottom: 0.3rem; color: var(--text-secondary); font-size: 1rem; }
.lap-stats dd { margin: 0; font-size: 1.125rem; font-weight: 400; overflow-wrap: anywhere; }
.best-index { font-size: 1rem; color: var(--text-secondary); }
.lap-table-scroll { margin: 0 1rem 1rem; overflow-x: auto; }
.lap-list { width: 100%; border-collapse: collapse; }
.lap-list th,
.lap-list td {
  text-align: left;
  white-space: nowrap;
  padding: 0.55rem 0.4rem;
  border-bottom: 1px solid var(--border-color);
  font-size: 1.05rem;
}
.lap-list th { font-size: 1rem; color: var(--text-secondary); font-weight: 600; }
.lap-list th:first-child,
.lap-list td:first-child { width: 2.5rem; }
.lap-list td { font-weight: 400; }
.lap-list tr:last-child td { border-bottom: 0; }
.lap-list tr.best { color: var(--accent-success); }
.lap-list td.slower { color: var(--accent-danger); }
.actions {
  text-align: right;
}
</style>
