<script setup>
import { computed } from "vue";
import { useDeviceStore } from "../stores/device";
import NodeDiagnostics from "./NodeDiagnostics.vue";
import NodeAge from "./NodeAge.vue";
import MasterConnectionLight from "./MasterConnectionLight.vue";
import { USB_PROTO_VERSION, RADIO_PROTO_VERSION } from "../lib/protocol";

const device = useDeviceStore();
const versions = computed(() => {
  const id = device.identity;
  return [
    { label: "Firmware", value: id?.fw ?? "…" },
    { label: "USB", value: id?.usbProto != null ? `v${id.usbProto}` : "—", mismatch: id && id.usbProto !== USB_PROTO_VERSION },
    { label: "Radio", value: id?.radioProto != null ? `v${id.radioProto}` : "—", mismatch: id && id.radioProto !== RADIO_PROTO_VERSION },
  ];
});
const deviceId = computed(() => {
  const id = device.identity?.devid;
  if (!/^[0-9a-f]{16}$/i.test(id ?? "")) return "…";
  const low = id.slice(8).toUpperCase();
  const high = id.slice(0, 8).toUpperCase();
  // Match the firmware's node_sender_id(), including its zero-word fallback.
  return low !== "00000000" ? low : high !== "00000000" ? high : "00000001";
});

function disconnect() {
  if (window.confirm("Disconnect from the master?")) device.disconnect();
}
</script>

<template>
  <div class="card">
    <div class="card-header master-header">
      <h3><span class="heading-icon" aria-hidden="true">📡</span><span>Master</span><MasterConnectionLight class="dot" /></h3>
      <button v-if="!device.connected" class="btn btn-success btn-sm conn-btn" :disabled="device.connecting || !device.supported" @click="device.connect()">Connect</button>
      <button v-else class="btn btn-ghost btn-sm conn-btn disconnect-btn" @click="disconnect">Disconnect</button>
    </div>
    <div class="card-body">
      <p v-if="!device.supported" class="hint">Web Serial is not supported by this browser</p>
      <p v-else-if="!device.connected" class="hint">Connect the master over USB</p>

      <div v-if="device.connected" class="device-heading">
        <h4 class="device-id mono">{{ deviceId }}</h4>
        <NodeAge node-id="0" />
        <div class="device-versions" aria-label="Firmware and Protocol Versions">
          <span v-for="version in versions" :key="version.label" class="version-item">
            <span>{{ version.label }}</span>
            <span class="version-value" :class="{ mismatch: version.mismatch }">{{ version.value }}</span>
          </span>
        </div>
      </div>
      <div v-if="device.connected" class="master-details">
        <NodeDiagnostics node-id="0" />
      </div>
    </div>
  </div>
</template>

<style scoped>
.master-header {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.75rem 1.5rem;
}
.conn-btn {
  margin-left: auto;
}
.disconnect-btn:hover:not(:disabled), .disconnect-btn:focus-visible {
  color: #fff;
  background: var(--accent-danger);
}
.hint {
  font-size: 1rem;
  color: var(--text-secondary);
}
.device-heading { display: flex; flex-wrap: wrap; align-items: center; gap: 0.75rem; margin-bottom: 1rem; }
.device-versions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 0.35rem 1rem; margin-left: auto; color: var(--text-secondary); font-size: 0.875rem; line-height: 1.5; }
.version-item { display: inline-flex; align-items: baseline; gap: 0.35rem; white-space: nowrap; }
.version-value { font-weight: 500; }
.version-value.mismatch { color: var(--accent-danger); }
.master-details { container: master-layout / inline-size; }
</style>
