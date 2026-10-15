<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useDeviceStore } from "../stores/device";

const device = useDeviceStore();
const now = ref(Date.now());
const state = computed(() => {
  if (device.connecting) return { tone: "connecting", label: "Connecting" };
  if (!device.connected) return { tone: "bad", label: "Disconnected" };
  return device.masterFresh(now.value)
    ? { tone: "ok", label: "Connected" }
    : { tone: "warn", label: "Connected — no data" };
});
let timer;
onMounted(() => { timer = setInterval(() => (now.value = Date.now()), 1000); });
onUnmounted(() => clearInterval(timer));
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
.connecting { background: var(--accent-pending); }
.warn { background: var(--accent-warning); }
.bad { background: var(--accent-danger); }
</style>
