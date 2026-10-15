<script setup>
import { computed, ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";
import { downloadBlob, timestampSlug } from "../lib/format";
import { logRecord } from "../lib/deviceLogFormat";

const device = useDeviceStore();
const notyf = useNotification();
const open = ref(false);
const confirmClear = ref(false);

const rows = computed(() => device.logEntries.map((e) => ({ ...logRecord(e), id: e.id })));

async function exportJson() {
  const entries = (await device.exportLog()).map(logRecord);
  downloadBlob(new Blob([JSON.stringify({ exported_at: new Date().toISOString(), entries }, null, 2)], { type: "application/json" }), `fsk-timekeeper-log-${timestampSlug()}.json`);
}

async function clearAll() {
  if (!confirmClear.value) {
    confirmClear.value = true;
    setTimeout(() => (confirmClear.value = false), 4000);
    return;
  }
  confirmClear.value = false;
  await device.clearLog();
  notyf.success("Device log cleared");
}
</script>

<template>
  <div class="card">
    <div class="card-header log-header" :class="{ 'is-open': open }" @click="open = !open">
      <h3><span class="heading-icon" aria-hidden="true">📋</span><span>Device Log</span> <span class="muted">{{ device.logEntries.length }} entries</span></h3>
      <span class="disclosure" :class="{ 'is-open': open }" aria-hidden="true">›</span>
    </div>
    <div v-if="open" class="card-body">
      <div class="toolbar">
        <button class="btn btn-ghost btn-sm" :disabled="!device.logEntries.length" @click="exportJson">Export JSON</button>
        <button class="btn btn-sm" :class="confirmClear ? 'btn-danger' : 'btn-ghost'" :disabled="!device.logEntries.length" @click="clearAll">{{ confirmClear ? "Confirm Clear" : "Clear" }}</button>
      </div>
      <div v-if="!device.logEntries.length" class="empty-state">No entries.</div>
      <div v-else class="table-scroll">
        <table class="log-table">
          <thead>
            <tr>
              <th>Time</th>
              <th>Type</th>
              <th>Code</th>
              <th>Board</th>
              <th>Line</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="e in rows" :key="e.id">
              <td class="mono nowrap">{{ e.time }}</td>
              <td>{{ e.type }}</td>
              <td class="mono">{{ e.code }}</td>
              <td class="mono">{{ e.board }}</td>
              <td class="text">{{ e.line }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
.log-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  cursor: pointer;
  user-select: none;
}
.muted {
  color: var(--text-tertiary);
  font-weight: 400;
  font-size: 0.8rem;
}
.log-header:not(.is-open) { border-bottom: 0; }
.toolbar {
  display: flex;
  gap: 0.5rem;
  margin-bottom: 0.75rem;
}
.log-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.8rem;
}
.log-table th,
.log-table td {
  padding: 0.35rem 0.6rem;
  text-align: left;
  border-bottom: 1px solid var(--border-color);
  vertical-align: top;
}
.log-table th {
  color: var(--text-tertiary);
  font-weight: 600;
}
.nowrap {
  white-space: nowrap;
}
.text {
  white-space: pre-line;
  min-width: 12rem;
  overflow-wrap: anywhere;
  color: var(--text-secondary);
}
</style>
