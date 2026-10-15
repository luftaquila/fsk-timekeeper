<script setup>
import { ref, computed, watch } from "vue";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";

const device = useDeviceStore();
const settings = useSettingsStore();
const node = ref("0D2243B0");
const fake = computed(() => device.fakeTransport());
const sensors = ref([]);
const gps = ref(null);
const open = ref(false);
const connectionBusy = ref(false);

// The fake transport keeps plain JS state; refresh after each simulator action.
function refresh() {
  sensors.value = fake.value ? [...fake.value.sensors.keys()] : [];
  gps.value = fake.value?.gps ?? null;
  if (!sensors.value.includes(node.value)) node.value = sensors.value[0] ?? "";
}
watch(fake, refresh, { immediate: true });

async function toggleConnection() {
  if (connectionBusy.value || device.updateInProgress || device.connecting || (device.connected && !fake.value)) return;
  connectionBusy.value = true;
  try {
    if (fake.value) await device.disconnect();
    else await device.connect({ kind: "fake" });
  } finally {
    connectionBusy.value = false;
  }
}
function act(fn) {
  const f = fake.value;
  if (!f) return;
  fn(f);
  refresh();
}
function addSensor() {
  act((f) => {
    let id;
    do {
      id = (0x10000000 + Math.floor(Math.random() * 0xefffffff)).toString(16).toUpperCase();
    } while (f.sensors.has(id));
    f.addSensor(id);
    node.value = id;
    device.transmitLine("?STATUS");
  });
}
function deleteSensor() {
  act((f) => {
    if (!f.removeSensor(node.value)) return;
    delete device.telemetry[node.value];
    settings.removeMapping(node.value);
  });
}
</script>

<template>
  <div class="card simulator">
    <div class="card-header simulator-toggle" :class="{ 'is-open': open }" role="button" tabindex="0" :aria-expanded="open" aria-controls="simulator-body" @click="open = !open" @keydown.enter.prevent="open = !open" @keydown.space.prevent="open = !open">
      <h3><span class="heading-icon" aria-hidden="true">🧪</span><span>Simulator (Development Build Only)</span></h3>
      <svg class="disclosure" :class="{ 'is-open': open }" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 6 6 6-6 6" /></svg>
    </div>
    <div v-if="open" id="simulator-body" class="card-body">
      <div class="connection-toolbar">
        <span class="group-title">Master</span>
        <div class="actions">
          <button class="btn" :class="fake ? 'btn-ghost' : 'btn-primary'" :disabled="connectionBusy || device.updateInProgress || device.connecting || (device.connected && !fake)" @click="toggleConnection">{{ fake ? "Disconnect" : "Connect Fake Master" }}</button>
        </div>
      </div>
      <fieldset class="simulator-controls" :disabled="!fake || connectionBusy" aria-label="Simulator Controls">
        <div class="sensor-toolbar">
          <label for="simulator-node" class="group-title">Sensor</label>
          <div class="actions">
            <select id="simulator-node" v-model="node" class="form-select node" :disabled="!sensors.length">
              <option v-if="!sensors.length" value="">No Sensors</option>
              <option v-for="s in sensors" :key="s" :value="s">{{ s }}</option>
            </select>
            <button class="btn btn-ghost" @click="addSensor">Add Sensor</button>
            <button class="btn btn-ghost" :disabled="!node" @click="deleteSensor">Delete Sensor</button>
          </div>
        </div>

        <section class="control-group" aria-labelledby="simulator-events-title">
          <h4 id="simulator-events-title" class="group-title">Sensor Events</h4>
          <fieldset class="actions" :disabled="!node" aria-label="Sensor Events">
            <button class="btn btn-success" @click="act((f) => f.crossing(node))">Crossing</button>
            <button class="btn btn-ghost" @click="act((f) => f.checkpoint(node))">Checkpoint</button>
          </fieldset>
        </section>

        <section class="control-group" aria-labelledby="simulator-faults-title">
          <h4 id="simulator-faults-title" class="group-title">Sensor Faults</h4>
          <fieldset class="actions" :disabled="!node" aria-label="Sensor Faults">
            <button class="btn btn-warning" @click="act((f) => f.loss(node, 1))">Loss</button>
            <button class="btn btn-warning" @click="act((f) => f.loss(node, 1, { unknownTime: true }))">Loss (Time Unknown)</button>
            <button class="btn btn-warning" @click="act((f) => f.injectBadLine(node))">Invalid Event</button>
            <button class="btn btn-warning" @click="act((f) => f.rebootSensor(node))">Reboot Sensor</button>
          </fieldset>
        </section>

        <section class="control-group" aria-labelledby="simulator-gps-title">
          <h4 id="simulator-gps-title" class="group-title">GPS &amp; PPS</h4>
          <div class="actions">
            <button class="btn btn-warning" @click="act((f) => f.setGps({ ppb: f.gps.ppb ? 0 : 100000 }))">GPS {{ gps?.ppb ? "→ 0" : "+100" }} ppm</button>
            <button class="btn btn-warning" @click="act((f) => f.setGps({ valid: !f.gps.valid, fix: f.gps.valid ? 0 : 1 }))">GPS {{ gps?.valid !== false ? "Off" : "On" }}</button>
            <button class="btn btn-warning" @click="act((f) => f.breakGpsSegment())">Break PPS Segment</button>
          </div>
        </section>

        <section class="control-group" aria-labelledby="simulator-master-title">
          <h4 id="simulator-master-title" class="group-title">Master &amp; Serial</h4>
          <div class="actions">
            <button class="btn btn-warning" @click="act((f) => f.corruptNext())">Corrupt Next Line</button>
            <button class="btn btn-danger" @click="act((f) => f.masterClockFault())">Master Clock Fault</button>
            <button class="btn btn-danger" @click="act((f) => f.rebootMaster())">Reboot Master</button>
          </div>
        </section>
      </fieldset>
    </div>
  </div>
</template>

<style scoped>
.simulator-toggle {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  cursor: pointer;
  user-select: none;
}
.simulator-toggle:focus-visible { outline: 2px solid var(--accent-primary); outline-offset: -3px; }
.simulator-toggle:not(.is-open) { border-bottom: 0; }
.connection-toolbar,
.sensor-toolbar,
.control-group {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  align-items: center;
  gap: 1.5rem;
}
.sensor-toolbar {
  padding-bottom: 1.5rem;
}
.connection-toolbar {
  padding-bottom: 1.5rem;
  margin-bottom: 1.5rem;
  border-bottom: 1px solid var(--border-color);
}
.simulator-controls {
  min-width: 0;
  border: 0;
  padding: 0;
  margin: 0;
}
.node {
  min-width: 0;
  width: auto;
  max-width: 100%;
  font-family: var(--font-mono);
}
.control-group {
  padding: 1.5rem 0;
  border-top: 1px solid var(--border-color);
}
.control-group:last-child {
  padding-bottom: 0;
}
.group-title {
  font-size: 1.125rem;
  font-weight: 600;
}
.actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
  min-width: 0;
  border: 0;
  padding: 0;
  margin: 0;
}
.btn,
.node {
  line-height: 1.4;
}
.node {
  min-height: 44px;
  padding: 0.625rem 0.75rem;
  font-size: 1rem;
}
@media (max-width: 640px) {
  .connection-toolbar,
  .sensor-toolbar,
  .control-group {
    grid-template-columns: minmax(0, 1fr);
    gap: 1rem;
  }
}
</style>
