<script setup>
import { ref, computed } from "vue";
import { useDeviceStore } from "../stores/device";

const device = useDeviceStore();
const node = ref("0D2243B0");
const fake = computed(() => device.fakeTransport());
const sensors = computed(() => (fake.value ? [...fake.value.sensors.keys()] : []));

function connectFake() {
  device.connect({ kind: "fake" });
}
function act(fn) {
  const f = fake.value;
  if (!f) return;
  fn(f);
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🧪 Simulator (development build only)</h3></div>
    <div class="card-body">
      <div class="row">
        <button v-if="!fake" class="btn btn-primary btn-sm" :disabled="device.connected" @click="connectFake">Connect fake master</button>
        <template v-else>
          <select v-model="node" class="form-select node">
            <option v-for="s in sensors" :key="s" :value="s">{{ s }}</option>
          </select>
          <button class="btn btn-success btn-sm" @click="act((f) => f.crossing(node))">Crossing</button>
          <button class="btn btn-ghost btn-sm" @click="act((f) => f.checkpoint(node))">Checkpoint</button>
          <button class="btn btn-warning btn-sm" @click="act((f) => f.loss(node, 1))">Loss</button>
          <button class="btn btn-warning btn-sm" @click="act((f) => f.loss(node, 1, { unknownTime: true }))">Loss (time unknown)</button>
          <button class="btn btn-warning btn-sm" @click="act((f) => f.injectBadLine(node))">Invalid event</button>
          <button class="btn btn-warning btn-sm" @click="act((f) => f.corruptNext())">Corrupt next line</button>
          <button class="btn btn-warning btn-sm" @click="act((f) => f.rebootSensor(node))">Reboot sensor</button>
          <button class="btn btn-danger btn-sm" @click="act((f) => f.masterClockFault())">Master clock fault</button>
          <button class="btn btn-danger btn-sm" @click="act((f) => f.rebootMaster())">Reboot master</button>
          <button class="btn btn-ghost btn-sm" @click="act((f) => f.addSensor((0x10000000 + Math.floor(Math.random() * 0xefffffff)).toString(16).toUpperCase()))">Add sensor</button>
          <button class="btn btn-ghost btn-sm" @click="act((f) => f.setGps({ ppb: f.gps.ppb ? 0 : 100000 }))">GPS {{ fake.gps.ppb ? "→ 0" : "+100" }} ppm</button>
          <button class="btn btn-ghost btn-sm" @click="act((f) => f.setGps({ valid: !f.gps.valid, fix: f.gps.valid ? 0 : 1 }))">GPS {{ fake.gps.valid ? "off" : "on" }}</button>
          <button class="btn btn-ghost btn-sm" @click="act((f) => f.breakGpsSegment())">Break PPS segment</button>
        </template>
      </div>
    </div>
  </div>
</template>

<style scoped>
.row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}
.node {
  width: auto;
  font-family: var(--font-mono);
}
</style>
