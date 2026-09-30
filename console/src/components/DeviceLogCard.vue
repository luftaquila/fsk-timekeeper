<script setup>
import { ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";
import { downloadBlob, timestampSlug, fmtDateTime } from "../lib/format";

const device = useDeviceStore();
const notyf = useNotification();
const open = ref(false);
const confirmClear = ref(false);

const KIND_LABEL = { X: "Error", boot: "Boot", console: "Console" };

async function exportJson() {
  const entries = await device.exportLog();
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
    <div class="card-header log-header" @click="open = !open">
      <h3>📋 Device log <span class="muted">{{ device.logEntries.length }} entries</span></h3>
      <span class="chev">{{ open ? "▾" : "▸" }}</span>
    </div>
    <div v-if="open" class="card-body">
      <div class="toolbar">
        <button class="btn btn-ghost btn-sm" :disabled="!device.logEntries.length" @click="exportJson">Export JSON</button>
        <button class="btn btn-sm" :class="confirmClear ? 'btn-danger' : 'btn-ghost'" :disabled="!device.logEntries.length" @click="clearAll">{{ confirmClear ? "Confirm clear" : "Clear" }}</button>
      </div>
      <div v-if="!device.logEntries.length" class="empty-state">No entries.</div>
      <div v-else class="table-scroll">
        <table class="log-table">
          <thead>
            <tr>
              <th>Time</th>
              <th>Kind</th>
              <th>Code</th>
              <th>Board</th>
              <th>Line</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="e in device.logEntries" :key="e.id">
              <td class="mono nowrap">{{ fmtDateTime(e.at) }}</td>
              <td>{{ KIND_LABEL[e.kind] || e.kind }}</td>
              <td class="mono">{{ e.code }}</td>
              <td class="mono">{{ e.devid || "—" }}</td>
              <td class="mono text">{{ e.text }}</td>
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
  cursor: pointer;
  user-select: none;
}
.muted {
  color: var(--text-tertiary);
  font-weight: 400;
  font-size: 0.8rem;
}
.chev {
  color: var(--text-tertiary);
}
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
  word-break: break-all;
  color: var(--text-secondary);
}
</style>
