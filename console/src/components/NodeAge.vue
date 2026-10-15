<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useDeviceStore } from "../stores/device";
import { telemetryAgeMs, telemetryLinkState } from "../lib/quality";
import { fmtAgeMs } from "../lib/format";

const props = defineProps({ nodeId: { type: String, required: true } });
const device = useDeviceStore();
const now = ref(Date.now());
const label = computed(() => props.nodeId === "0" ? "Report Age" : "Last Heard");
const tone = computed(() => telemetryLinkState(device.telemetry[props.nodeId], now.value) === "online" ? "badge-success" : "badge-warning");
const age = computed(() => {
  const report = device.telemetry[props.nodeId];
  if (props.nodeId === "0") return fmtAgeMs(telemetryAgeMs(report, now.value));
  return report?.last_seen_at == null ? "—" : fmtAgeMs(Math.max(0, now.value - report.last_seen_at));
});
let timer;
onMounted(() => { timer = setInterval(() => (now.value = Date.now()), 1000); });
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <span class="node-age badge mono" :class="tone" :aria-label="`${label}: ${age}`">{{ age }}</span>
</template>

<style scoped>
.node-age {
  justify-content: center;
  white-space: nowrap;
  font-family: var(--font-sans);
  font-size: 0.875rem;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
  line-height: 1.25rem;
  padding: 0.125rem 0.5rem;
  border-radius: 999px;
}
.node-age.badge-success {
  color: color-mix(in srgb, var(--accent-success) 60%, var(--text-primary));
  background: color-mix(in srgb, var(--accent-success) 18%, var(--bg-primary));
}
.node-age.badge-warning {
  color: color-mix(in srgb, var(--accent-warning) 50%, var(--text-primary));
  background: color-mix(in srgb, var(--accent-warning) 20%, var(--bg-primary));
}
</style>
