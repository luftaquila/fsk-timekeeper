<script setup>
import { ref, computed, nextTick, watch, onBeforeUnmount } from "vue";
import { useTimingStore } from "../stores/timing";
import { useDeviceStore } from "../stores/device";
import { useHistoryStore } from "../stores/history";
import { useNotification } from "../composables/useNotification";
import { MODES, MODE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { formatDuration, fmtDateTime } from "../lib/format";
import { historyRecord, lapSummary, lapDelta, statusLabel, calibrationLabel } from "../lib/historyPresentation";

import FaultReasons from "../components/FaultReasons.vue";
import LabeledValue from "../components/LabeledValue.vue";

const history = useHistoryStore();
const notyf = useNotification();
const timing = useTimingStore();
const device = useDeviceStore();
const importInput = ref(null);
const importBlocked = computed(() => exporting.value || history.importing || history.clearing || timing.armed || timing.starting || timing.classifying || device.updateInProgress);
const classifying = ref(false);
const exporting = ref(false);
const filter = ref("");
const lapDialog = ref(null);
const lapPopup = ref(null);
const backupMenu = ref(null);
const backupOpen = ref(false);

const rows = computed(() => history.filtered(filter.value || null));

function result(r) {
  return historyRecord(r, history.edgesFor(r)).Result;
}
function lapStats(r) {
  return lapSummary(r, history.edgesFor(r));
}
const lapSummaries = computed(() => new Map(rows.value.map((r) => [r.runId, lapStats(r)])));
const calibrations = computed(() => new Map(rows.value.map((r) => [r.runId, calibrationLabel(r, history.edgesFor(r))])));

async function openLaps(r) {
  const stats = lapSummaries.value.get(r.runId);
  if (!stats) return;
  lapPopup.value = { ...stats, note: r.note, date: fmtDateTime(r.startedAt) };
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
    await timing.classify(event.target.value || null, { runId: r.runId });
  } finally {
    event.target.value = history.status(history.rows.find((row) => row.runId === r.runId) || r);
    classifying.value = false;
  }
}
async function exportFile(format) {
  if (exporting.value || history.clearing) return;
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
    const { added, skipped } = await history.importJson(file);
    notyf.success(`Restored ${added} records; ${skipped} duplicates skipped`);
  } catch (error) { notyf.error(error.message || "Restore failed"); }
}

function closeBackupOnOutside(event) {
  if (!backupMenu.value?.contains(event.target)) backupOpen.value = false;
}
watch(backupOpen, (open) => {
  if (open) document.addEventListener("pointerdown", closeBackupOnOutside);
  else document.removeEventListener("pointerdown", closeBackupOnOutside);
});
onBeforeUnmount(() => document.removeEventListener("pointerdown", closeBackupOnOutside));
function downloadBackup() {
  backupOpen.value = false;
  exportFile("json");
}
function restoreBackup() {
  backupOpen.value = false;
  importInput.value.click();
}

async function remove(r) {
  try { await history.removeRow(r.runId); notyf.success("Deleted"); }
  catch (error) { notyf.error(error.message || "Delete failed"); }
}
async function clearAll() {
  if (importBlocked.value || classifying.value || device.connecting || device.dfuInProgress) return;
  if (!window.confirm("Delete all history, events, PPS data, quarantined data, and device logs? The master will be disconnected. Settings will be kept.")) return;
  try { await history.clearAll(); notyf.success("All recorded data cleared"); }
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
      <button class="btn btn-ghost btn-sm" :disabled="!rows.length || exporting || history.importing || history.clearing" @click="exportFile('excel')"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3" /></svg> Export Excel</button>
      <div ref="backupMenu" class="backup-menu" @keydown.esc="backupOpen = false">
        <button class="btn btn-ghost btn-sm" :disabled="exporting || history.importing || history.clearing" aria-controls="backup-actions" :aria-expanded="backupOpen" @click="backupOpen = !backupOpen"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 8v13H3V8M1 3h22v5H1zM10 12h4" /></svg> Backup <svg aria-hidden="true" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6" /></svg></button>
        <div v-show="backupOpen" id="backup-actions" class="backup-actions">
          <button class="btn btn-ghost btn-sm" :disabled="exporting || history.importing || history.clearing" @click="downloadBackup"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3" /></svg> Download</button>
          <button class="btn btn-ghost btn-sm" :disabled="importBlocked" @click="restoreBackup"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 8l5-5 5 5M12 3v12" /></svg> Restore</button>
        </div>
      </div>
      <input ref="importInput" type="file" accept=".json,application/json" hidden @change="importFile" />
      <button class="btn btn-ghost btn-ghost-danger btn-sm" :disabled="importBlocked || classifying || device.connecting || device.dfuInProgress" @click="clearAll"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M19 6l-1 14H6L5 6M9 6V3h6v3M10 10v6M14 10v6" /></svg> Clear All</button>
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
              <th class="centered">Calibration</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <template v-for="r in rows" :key="r.runId">
              <tr>
                <td class="mono nowrap">{{ fmtDateTime(r.startedAt) }}</td>
                <td class="centered"><span class="badge mode-chip" :class="{ 'mode-sprint': r.mode === 'sprint', 'mode-laps': r.mode === 'laps' }">{{ MODE_LABEL[r.mode] || r.mode }}</span></td>
                <td class="note-cell">{{ r.note || "-" }}</td>
                <td class="mono nowrap result-cell centered">
                  <button v-if="lapSummaries.get(r.runId)" class="result-link" aria-haspopup="dialog" @click="openLaps(r)">{{ result(r) }}</button>
                  <span v-else>{{ result(r) }}</span>
                </td>
                <td class="centered">
                  <select class="badge status-select" :class="badgeClass(r)" :value="history.status(r)"
                    aria-label="Record status" :disabled="classifying || timing.starting || timing.classifying || history.importing || history.clearing" @change="classify(r, $event)">
                    <option v-if="!RESULT_CLASSIFICATIONS.includes(history.status(r))" :value="history.status(r)" disabled>{{ statusLabel(r) }}</option>
                    <option v-if="r.classification" value="">Clear</option>
                    <option v-for="value in RESULT_CLASSIFICATIONS" :key="value" :value="value">{{ value }}</option>
                  </select>
                  <span v-if="r.durable === false" class="badge badge-danger">Not Saved</span>
                </td>
                <td class="centered">
                  <span v-if="calibrations.get(r.runId) !== '-'" class="badge" :class="calibrations.get(r.runId) === 'GPS' ? 'badge-success' : 'badge-warning'">{{ calibrations.get(r.runId) }}</span>
                  <span v-else>-</span>
                </td>
                <td class="actions"><button class="btn btn-ghost btn-sm" :disabled="history.importing || history.clearing" @click="remove(r)">Delete</button></td>
              </tr>
              <tr v-if="r.fault?.reasons?.length" class="fault-row"><td colspan="7"><FaultReasons :reasons="r.fault.reasons" :boundary-tick="r.boundaryTick" /></td></tr>
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
            <div class="lap-context">
              <LabeledValue label="Date" :value="lapPopup.date" text />
              <LabeledValue v-if="lapPopup.note" label="Note" :value="lapPopup.note" text />
            </div>
          </div>
          <button class="btn btn-ghost" aria-label="Close lap details" autofocus @click="closeLaps">✕</button>
        </div>
        <dl class="lap-stats">
          <div><dt>Laps</dt><dd class="mono">{{ lapPopup.laps.length }}</dd></div>
          <div><dt>Best</dt><dd class="mono">{{ formatDuration(lapPopup.best) }} <span class="best-index">(#{{ lapPopup.bestIndex + 1 }})</span></dd></div>
          <div><dt>Average</dt><dd class="mono">{{ formatDuration(lapPopup.sum, BigInt(lapPopup.laps.length)) }}</dd></div>
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
.backup-menu {
  position: relative;
}
.backup-actions {
  position: absolute;
  top: calc(100% + 0.25rem);
  right: 0;
  z-index: 10;
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
  min-width: 100%;
  padding: 0.25rem;
  background: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: 8px;
  box-shadow: var(--shadow-hover);
}
.backup-actions .btn {
  justify-content: flex-start;
  border: 0;
  white-space: nowrap;
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
.lap-context { margin-top: 0.35rem; display: flex; flex-wrap: wrap; gap: 0.35rem 1.25rem; }
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
