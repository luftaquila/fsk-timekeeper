<script setup>
/* The camera's state for the Camera Settings heading: Pi connected and camera session live. */
import { computed } from "vue";
import { useDeviceStore } from "../stores/device";
import { useCameraStore } from "../stores/camera";

const device = useDeviceStore();
const camera = useCameraStore();
const state = computed(() => {
  if (device.connecting && device.transportKind === "pi") return { tone: "connecting", label: "Connecting" };
  if (!camera.connected) return { tone: "bad", label: "Disconnected" };
  return device.telemetry[device.cameraNode]?.link_state === "online"
    ? { tone: "ok", label: "Camera Live" }
    : { tone: "warn", label: "Connected, No Camera Session" };
});
</script>

<template>
  <span class="connection-light" :class="state.tone" role="img" :aria-label="state.label"></span>
</template>

<style scoped>
.connection-light {
  flex-shrink: 0;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  display: inline-block;
}
.ok {
  background: var(--accent-success);
  box-shadow: 0 0 6px var(--accent-success);
}
.connecting { background: var(--accent-pending); }
.warn { background: var(--accent-warning); }
.bad { background: var(--accent-danger); }
</style>
