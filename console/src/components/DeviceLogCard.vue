<script setup>
import { computed, ref, watch } from "vue";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";
import { downloadBlob, timestampSlug } from "../lib/format";
import { LOG_TYPES, logRecord } from "../lib/deviceLogFormat";

const device = useDeviceStore();
const notyf = useNotification();
const open = ref(false);

const type = ref("");
const page = ref(1);
const pageSize = ref(10);
const types = computed(() => [...new Set([...Object.keys(LOG_TYPES), ...device.logEntries.map((e) => e.kind)])]
  .map((kind) => ({ kind, label: LOG_TYPES[kind] ?? kind })));
const filtered = computed(() => device.logEntries.filter((e) => !type.value || e.kind === type.value));
const pageCount = computed(() => Math.max(1, Math.ceil(filtered.value.length / pageSize.value)));
const start = computed(() => (page.value - 1) * pageSize.value);
const rows = computed(() => filtered.value.slice(start.value, start.value + pageSize.value)
  .map((e) => ({ ...logRecord(e), id: e.id })));
watch([type, pageSize], () => { page.value = 1; }, { flush: "sync" });
watch(pageCount, (count) => { page.value = Math.min(page.value, count); });

async function exportJson() {
  const selectedType = type.value;
  const entries = (await device.exportLog()).filter((e) => !selectedType || e.kind === selectedType).map(logRecord);
  downloadBlob(new Blob([JSON.stringify({ exported_at: new Date().toISOString(), entries }, null, 2)], { type: "application/json" }), `fsk-timekeeper-log-${timestampSlug()}.json`);
}

async function clearAll() {
  if (device.clearingData) return;
  if (!window.confirm("Delete all device log entries?")) return;
  await device.clearLog();
  notyf.success("Device log cleared");
}
</script>

<template>
  <div class="card">
    <div class="card-header log-header" :class="{ 'is-open': open }" @click="open = !open">
      <h3><span class="heading-icon" aria-hidden="true">📋</span><span>Device Log</span> <span class="count-chip" :aria-label="`${device.logEntries.length} entries`">{{ device.logEntries.length }}</span></h3>
      <svg class="disclosure" :class="{ 'is-open': open }" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 6 6 6-6 6" /></svg>
    </div>
    <div v-if="open" class="card-body">
      <div class="toolbar">
        <label class="type-filter">Type
          <select v-model="type" class="form-select" aria-label="Device log type">
            <option value="">All Types</option>
            <option v-for="option in types" :key="option.kind" :value="option.kind">{{ option.label }}</option>
          </select>
        </label>
        <button class="btn btn-ghost btn-sm" :disabled="!filtered.length || device.clearingData" aria-label="Export all logs matching the type filter as JSON" @click="exportJson"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3" /></svg> Export JSON</button>
        <button class="btn btn-ghost btn-ghost-danger btn-sm" :disabled="!device.logEntries.length || device.clearingData" @click="clearAll"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M19 6l-1 14H6L5 6M9 6V3h6v3M10 10v6M14 10v6" /></svg> Clear All</button>
      </div>
      <div v-if="!filtered.length" class="empty-state">{{ device.logEntries.length ? 'No matching entries' : 'No entries' }}</div>
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
      <nav class="pagination" aria-label="Device log pages">
        <label class="page-size">Rows per page
          <select v-model.number="pageSize" class="form-select" aria-label="Log rows per page">
            <option v-for="size in [10, 25, 50]" :key="size" :value="size">{{ size }}</option>
          </select>
        </label>
        <span class="page-range" aria-live="polite">{{ filtered.length ? start + 1 : 0 }}–{{ Math.min(start + pageSize, filtered.length) }} of {{ filtered.length }}</span>
        <div class="page-controls">
          <button class="btn btn-ghost btn-sm" :disabled="page === 1" aria-label="First log page" @click="page = 1">First</button>
          <button class="btn btn-ghost btn-sm" :disabled="page === 1" aria-label="Previous log page" @click="page--">Previous</button>
          <span>{{ page }} / {{ pageCount }}</span>
          <button class="btn btn-ghost btn-sm" :disabled="page === pageCount" aria-label="Next log page" @click="page++">Next</button>
          <button class="btn btn-ghost btn-sm" :disabled="page === pageCount" aria-label="Last log page" @click="page = pageCount">Last</button>
        </div>
      </nav>
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

.log-header:not(.is-open) { border-bottom: 0; }
.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
  margin-bottom: 0.75rem;
}
.type-filter, .page-size, .pagination, .page-controls {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}
.type-filter { margin-right: auto; }
.type-filter .form-select, .page-size .form-select { width: auto; }
.pagination { justify-content: space-between; margin-top: 0.75rem; }
.page-range { color: var(--text-secondary); }
.log-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 1rem;
}
.log-table th,
.log-table td {
  padding: 0.35rem 0.6rem;
  text-align: left;
  border-bottom: 1px solid var(--border-color);
  vertical-align: top;
}
.log-table th {
  color: var(--text-secondary);
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
