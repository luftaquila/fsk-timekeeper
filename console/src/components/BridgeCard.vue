<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useDeviceStore } from "../stores/device";
import { fmtAgeMs } from "../lib/format";

const device = useDeviceStore();
const now = ref(Date.now());
let timer = null;
onMounted(() => {
  timer = setInterval(() => (now.value = Date.now()), 1000);
});
onUnmounted(() => clearInterval(timer));

const fresh = computed(() => device.masterFresh(now.value));
const uptime = computed(() => (device.heartbeat ? fmtAgeMs(device.heartbeat.uptimeMs) : "—"));
const lastLine = computed(() => (device.connected ? fmtAgeMs(now.value - device.lastLineAt) : "—"));
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>📡 Master</h3></div>
    <div class="card-body">
      <div class="conn-row">
        <span class="dot" :class="device.connected ? (fresh ? 'ok' : 'warn') : 'bad'"></span>
        <span class="state">
          {{ device.connected ? (fresh ? "Connected" : "Connected — no data") : device.connecting ? "Connecting…" : "Disconnected" }}
          <span v-if="device.transportKind === 'fake'" class="badge badge-warning">simulator</span>
        </span>
        <button v-if="!device.connected" class="btn btn-success btn-sm conn-btn" :disabled="device.connecting || !device.supported" @click="device.connect()">Connect</button>
        <button v-else class="btn btn-ghost btn-sm conn-btn" @click="device.disconnect()">Disconnect</button>
      </div>
      <p v-if="!device.supported" class="hint">Web Serial is not available in this browser. Use Chrome or Edge on a desktop.</p>

      <dl v-if="device.connected" class="info">
        <dt>Firmware</dt>
        <dd class="mono">{{ device.identity ? `${device.identity.product} ${device.identity.fw}` : "…" }}</dd>
        <dt>Device ID</dt>
        <dd class="mono">{{ device.identity?.devid || "…" }}</dd>
        <dt>Radio</dt>
        <dd class="mono">{{ device.identity ? `${device.identity.freqMhz} MHz · SF${device.identity.sf} · BW${device.identity.bw}` : "…" }}</dd>
        <dt>Uptime</dt>
        <dd class="mono">{{ uptime }}</dd>
        <dt>Sensors seen</dt>
        <dd class="mono">{{ device.heartbeat?.nseen ?? "…" }}</dd>
        <dt>Last line</dt>
        <dd class="mono">{{ lastLine }} ago</dd>
      </dl>
    </div>
  </div>
</template>

<style scoped>
.conn-row {
  display: flex;
  align-items: center;
  gap: 0.6rem;
}
.state {
  flex: 1;
  font-weight: 600;
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  display: inline-block;
}
.dot.ok {
  background: var(--accent-success);
  box-shadow: 0 0 6px var(--accent-success);
}
.dot.warn {
  background: var(--accent-warning);
}
.dot.bad {
  background: var(--accent-danger);
}
.conn-btn {
  margin-left: auto;
}
.hint {
  margin-top: 0.6rem;
  font-size: 0.8rem;
  color: var(--text-tertiary);
}
.info {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: 0.25rem 1rem;
  margin-top: 1rem;
  font-size: 0.85rem;
}
.info dt {
  color: var(--text-tertiary);
}
.info dd {
  margin: 0;
}
</style>
