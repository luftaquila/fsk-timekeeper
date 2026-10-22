<script setup>
/* The camera's state for the Camera Settings heading, the one place it shows: the camera Pi link,
 * the camera at the Pi, and its session as the master hears it. */
import { computed } from "vue";
import { useDeviceStore } from "../stores/device";
import { useCameraStore } from "../stores/camera";

const device = useDeviceStore();
const camera = useCameraStore();
const LINK = { degraded: "STALE", lost: "LOST" };
const state = computed(() => {
  if (device.connecting && device.transportKind === "pi") return { tone: "connecting", label: "Connecting" };
  if (!camera.connected) return { tone: "bad", label: "Disconnected" };
  if (!camera.info) return { tone: "connecting", label: "Connecting" };
  if (!camera.info.camera?.connected) return { tone: "bad", label: "Camera Offline" };
  if (!camera.info.session?.running) return { tone: "idle", label: "Camera Standby" };
  const link = device.telemetry[device.cameraNode]?.link_state;
  return link === "online" ? { tone: "ok", label: "Camera Live" } : { tone: "warn", label: `Camera Link: ${LINK[link] ?? "NONE"}` };
});
</script>

<template>
  <span class="connection-light" :class="state.tone" role="img" :aria-label="state.label" :title="state.label"></span>
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
.idle { background: var(--accent-primary); }
.connecting { background: var(--accent-pending); }
.warn { background: var(--accent-warning); }
.bad { background: var(--accent-danger); }
</style>
