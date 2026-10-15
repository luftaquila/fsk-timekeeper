<script setup>
import { computed } from "vue";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";
import { useTimingStore } from "../stores/timing";
import { ROLES, ROLE_LABEL } from "../lib/constants";
import NodeDiagnostics from "./NodeDiagnostics.vue";
import NodeAge from "./NodeAge.vue";

const device = useDeviceStore();
const settings = useSettingsStore();
const timing = useTimingStore();

function digitsOnly(e) {
  e.target.value = e.target.value.replace(/\D/g, "");
}

function onDebounce(e) {
  settings.setDebounceMs(e.target.value);
  e.target.value = settings.state.debounceMs;
}

// Discovered nodes = telemetry ∪ saved mapping, master excluded.
const nodes = computed(() => {
  const s = new Set([...Object.keys(device.telemetry), ...settings.mappingList.map((m) => m.node_id)]);
  s.delete("0");
  return [...s].sort();
});

function updateMapping(node, changes) {
  const current = settings.mappingOf(node) ?? { role: "unused", note: "" };
  settings.setMapping(node, { ...current, ...changes });
}

</script>

<template>
  <div class="card sensor-card">
    <div class="card-header mapping-header">
      <h3><span class="heading-icon" aria-hidden="true">🔭</span><span>Sensors</span> <span class="sensor-count count-chip">{{ nodes.length }}</span></h3>
      <label class="debounce-control">
        <span>Debounce</span>
        <input type="text" inputmode="numeric" class="form-input debounce-input" :value="settings.state.debounceMs" :disabled="timing.armed" @input="digitsOnly" @change="onDebounce" />
        <span>ms</span>
      </label>
    </div>
    <div v-if="!nodes.length" class="card-body empty-state">{{ device.connected ? "No Sensors Detected" : "Connect the master to detect sensors" }}</div>
    <div v-else class="sensor-list">
      <article v-for="node in nodes" :key="node" class="sensor" :aria-label="`Sensor ${node}`">
        <div class="sensor-heading">
          <div class="sensor-title"><h4 class="device-id mono">{{ node }}</h4><NodeAge :node-id="node" /></div>
          <div class="mapping-controls">
            <label class="sensor-role">
              <span>Role</span>
              <select :value="settings.mappingOf(node)?.role ?? 'unused'" class="form-select" aria-label="Role" @change="updateMapping(node, { role: $event.target.value })">
                <option v-for="r in ROLES" :key="r" :value="r">{{ ROLE_LABEL[r] }}</option>
              </select>
            </label>
            <label class="sensor-note">
              <input :value="settings.mappingOf(node)?.note ?? ''" class="form-input" aria-label="Note" placeholder="Note" @input="updateMapping(node, { note: $event.target.value })" />
            </label>
          </div>
        </div>
        <NodeDiagnostics :node-id="node" />
      </article>
    </div>
  </div>
</template>

<style scoped>
.mapping-header, .sensor-heading {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.75rem 1.5rem;
}
.debounce-control {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  color: var(--text-secondary);
  font-size: 1rem;
}
.debounce-input { width: 5rem; padding: 0.4rem 0.5rem; text-align: right; }
.sensor { padding: 1.25rem; min-width: 0; container: sensor-layout / inline-size; }
.sensor + .sensor { border-top: 1px solid var(--border-color); }
.sensor-heading { margin-bottom: 1rem; }

.sensor-title { display: flex; align-items: center; gap: 0.75rem; flex-shrink: 0; }
.mapping-controls {
  display: flex;
  flex: 1 1 18rem;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 0.75rem 15px;
  align-items: center;
  min-width: 0;
}
.mapping-controls label { display: flex; align-items: center; gap: 0.5rem; min-width: 0; color: var(--text-secondary); font-size: 1rem; }
.mapping-controls label > span { flex-shrink: 0; }
.mapping-controls .form-input, .mapping-controls .form-select { height: 2.5rem; padding: 0.375rem 0.625rem; font-size: 1rem; }
.mapping-controls .form-select { width: 6.5rem; }
.mapping-controls .sensor-role { color: var(--text-primary); font-size: 1rem; font-weight: 600; }
.sensor-role .form-select { width: 7.25rem; font-weight: 600; color: var(--text-primary); }
.sensor-note { flex: 0 1 8rem; max-width: 100%; }
.sensor-note .form-input { min-width: 0; width: 100%; }
@container sensor-layout (min-width: 888px) {
  .sensor-heading {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    column-gap: 1.5rem;
  }
  .sensor-title { grid-column: 1 / 3; }
  .mapping-controls { grid-column: 3 / -1; flex-wrap: nowrap; justify-content: flex-start; }
  .mapping-controls > label:first-child { flex-shrink: 0; }
  .sensor-note { flex: 1 1 0; max-width: none; }
}
</style>
