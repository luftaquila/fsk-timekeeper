<script setup>
import { ref, computed } from "vue";
import { useHistoryStore } from "../stores/history";
import { useNotification } from "../composables/useNotification";
import { MODES, MODE_LABEL } from "../lib/constants";
import { formatDuration, fmtDateTime, fmtPpm } from "../lib/format";
import { resultNs, lapsNs, calibrationMethod, calibrationLabel, isOldFormatRow } from "../lib/results";

const history = useHistoryStore();
const notyf = useNotification();
const filter = ref("");
const withEvidence = ref(false);
const confirmClear = ref(false);

const rows = computed(() => history.filtered(filter.value || null));

function result(r) {
  const ns = resultNs(r, history.edgesFor(r));
  return ns != null ? formatDuration(ns) : "—";
}
function laps(r) {
  return lapsNs(r, history.edgesFor(r))
    .map((ns) => formatDuration(ns))
    .join(" / ");
}
function calibration(r) {
  if (isOldFormatRow(r)) return r.ppb != null ? `HFXO ${fmtPpm(r.ppb)} (GPS)` : "nominal 16 MHz";
  return calibrationLabel(calibrationMethod(r, history.edgesFor(r)));
}
function badgeClass(r) {
  return r.verification === "verified" ? "badge-success" : r.verification === "invalid" ? "badge-danger" : r.verification === "dnf" ? "badge-default" : "badge-warning";
}
async function remove(r) {
  await history.removeRow(r.id);
  notyf.success("Deleted");
}
async function clearAll() {
  if (!confirmClear.value) {
    confirmClear.value = true;
    setTimeout(() => (confirmClear.value = false), 4000);
    return;
  }
  confirmClear.value = false;
  await history.clearAll();
  notyf.success("History cleared");
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
      <label class="opt"><input type="checkbox" v-model="withEvidence" /> include raw evidence (JSON)</label>
      <button class="btn btn-ghost btn-sm" :disabled="!rows.length" @click="history.exportCsv(filter || null)">Export CSV</button>
      <button class="btn btn-ghost btn-sm" :disabled="!rows.length" @click="history.exportJson(filter || null, { withEvidence })">Export JSON</button>
      <button class="btn btn-sm" :class="confirmClear ? 'btn-danger' : 'btn-ghost'" :disabled="!history.rows.length" @click="clearAll">{{ confirmClear ? "Confirm clear all" : "Clear all" }}</button>
    </div>

    <div class="card">
      <div v-if="!rows.length" class="empty-state">No runs recorded yet.</div>
      <div v-else class="table-scroll">
        <table class="data-table">
          <thead>
            <tr>
              <th>Date</th>
              <th class="nowrap">Start (UTC)</th>
              <th>Mode</th>
              <th>Note</th>
              <th>Result</th>
              <th>Laps</th>
              <th>Status</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="r in rows" :key="r.id">
              <td class="mono nowrap">{{ fmtDateTime(r.createdAt) }}</td>
              <td class="mono nowrap" :title="calibration(r)">{{ r.startedUtc ? r.startedUtc.slice(11, 19) + "Z" : "—" }}</td>
              <td>{{ MODE_LABEL[r.mode] || r.mode }}</td>
              <td>{{ r.note || "—" }}</td>
              <td class="mono nowrap strong" :title="calibration(r)">{{ result(r) }}</td>
              <td class="mono laps">{{ laps(r) || "—" }}</td>
              <td>
                <span class="badge" :class="badgeClass(r)" :title="r.fault?.reasons?.map((x) => x.reason).join('\n') || ''">{{ history.status(r) }}</span>
                <span v-if="r.durable === false" class="badge badge-danger" title="Not stored durably">memory</span>
              </td>
              <td class="actions"><button class="btn btn-ghost btn-sm" @click="remove(r)">Delete</button></td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
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
.toolbar .form-select {
  width: auto;
}
.spacer {
  flex: 1;
}
.opt {
  display: flex;
  align-items: center;
  gap: 0.4rem;
  font-size: 0.8rem;
  color: var(--text-secondary);
}
.muted {
  color: var(--text-tertiary);
  font-size: 0.85rem;
}
.nowrap {
  white-space: nowrap;
}
.strong {
  font-weight: 700;
}
.laps {
  font-size: 0.8rem;
  color: var(--text-secondary);
  max-width: 28rem;
  white-space: normal;
}
.actions {
  text-align: right;
}
</style>
