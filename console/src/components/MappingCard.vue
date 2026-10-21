<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";
import { useTimingStore } from "../stores/timing";
import { telemetryFresh } from "../lib/quality";
import NodeDiagnostics from "./NodeDiagnostics.vue";
import NodeAge from "./NodeAge.vue";
import NodeMapping from "./NodeMapping.vue";

const device = useDeviceStore();
const settings = useSettingsStore();
const timing = useTimingStore();
const now = ref(Date.now());
let timer;
onMounted(() => { timer = setInterval(() => (now.value = Date.now()), 1000); });
onUnmounted(() => clearInterval(timer));
const removalBlocked = computed(() => timing.armed || timing.starting || timing.classifying || device.updateInProgress);

function digitsOnly(e) {
  e.target.value = e.target.value.replace(/\D/g, "");
}

function onDebounce(e) {
  settings.setDebounceMs(e.target.value);
  e.target.value = settings.state.debounceMs;
}

// Discovered nodes: telemetry ∪ admission (J) ∪ saved mapping, minus removed sensors and master.
const nodes = computed(() => {
  const s = new Set([...Object.keys(device.telemetry), ...Object.keys(device.admission), ...settings.mappingList.map((m) => m.node_id)]);
  s.delete("0");
  return [...s].filter((node) => !settings.state.removedSensors.includes(node)).sort();
});

function missing(node, at = now.value) {
  const report = device.telemetry[node];
  return !device.connected || !telemetryFresh(report, at) || report.link_state === "lost";
}

function removeSensor(node) {
  if (removalBlocked.value || !missing(node, Date.now())) return;
  settings.removeSensor(node);
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
    <p v-if="device.admissionNotice" class="admission-message" role="status">{{ device.admissionNotice }}</p>
    <div v-if="!nodes.length" class="card-body empty-state">{{ device.connected ? "No Sensors Detected" : "Connect the master to detect sensors" }}</div>
    <div v-else class="sensor-list">
      <article v-for="node in nodes" :key="node" class="sensor" :aria-label="`Sensor ${node}`">
        <div class="sensor-heading">
          <div class="sensor-title">
            <h4 class="device-id mono">{{ node }}</h4>
            <NodeAge :node-id="node" />
            <button v-if="missing(node)" class="btn btn-ghost btn-sm remove-sensor" type="button" :disabled="removalBlocked" @click="removeSensor(node)">Remove</button>
          </div>
          <NodeMapping :node-id="node" />
        </div>
        <NodeDiagnostics :node-id="node" />
      </article>
    </div>
  </div>
</template>

<style scoped>
.admission-message { padding: 0 1.25rem; }
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
.remove-sensor { color: var(--accent-danger); }
.remove-sensor:hover:not(:disabled), .remove-sensor:focus-visible { color: white; background: var(--accent-danger); border-color: var(--accent-danger); }

.sensor-title { display: flex; flex-wrap: wrap; align-items: center; gap: 0.75rem; flex-shrink: 0; max-width: 100%; }
@container sensor-layout (min-width: 888px) {
  .sensor-heading {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    column-gap: 1.5rem;
  }
  .sensor-title { grid-column: 1 / 3; }
}
</style>
