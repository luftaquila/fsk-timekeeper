<script setup>
/* Live view from the camera Pi (/api/live): the latest lines downscaled, with focus and exposure
 * figures, for aiming, focusing and setting the exposure. Off unless started. */
import { onBeforeUnmount, ref, watch } from "vue";
import MetricValue from "./MetricValue.vue";
import { useCameraStore } from "../stores/camera";

const camera = useCameraStore();
const on = ref(false);
const src = ref(null);
const stats = ref(null);
let ws = null;

function stop() {
  on.value = false;
  ws?.close();
  ws = null;
  if (src.value) URL.revokeObjectURL(src.value);
  src.value = null;
}

function start() {
  if (!camera.api) return;
  stop();
  on.value = true;
  ws = new WebSocket(camera.api.liveUrl());
  ws.binaryType = "blob";
  ws.onmessage = (event) => {
    if (typeof event.data === "string") {
      try { stats.value = JSON.parse(event.data); } catch { /* ignore */ }
      return;
    }
    const url = URL.createObjectURL(event.data);
    if (src.value) URL.revokeObjectURL(src.value);
    src.value = url;
  };
  ws.onclose = () => { if (on.value) stop(); };
}

watch(() => camera.connected, (connected) => { if (!connected) stop(); });
onBeforeUnmount(stop);
</script>

<template>
  <section class="settings-section" aria-labelledby="camera-live-title">
    <div class="section-heading">
      <h4 id="camera-live-title">Live View</h4>
    </div>
    <div class="section-content">
      <img v-if="src" :src="src" class="live" alt="Latest camera lines" />
      <dl class="live-stats">
        <div><dt>Focus</dt><dd><MetricValue :value="stats?.focus != null ? stats.focus.toFixed(1) : '-'" /></dd></div>
        <div v-for="[label, value] in [['Mean', stats?.mean], ['Max', stats?.max]]" :key="label"><dt>{{ label }}</dt><dd class="level">
          <template v-if="value != null">
            <MetricValue :value="value" /><span class="scale">/</span><MetricValue :value="255" /><MetricValue :value="Math.round((value / 255) * 100)" unit="%" parenthesized />
          </template>
          <MetricValue v-else value="-" />
        </dd></div>
        <div><dt>Saturated</dt><dd><MetricValue :value="stats?.saturation != null ? (stats.saturation * 100).toFixed(2) : '-'" :unit="stats?.saturation != null ? '%' : ''" /></dd></div>
      </dl>
      <div class="section-actions">
        <div class="apply-group">
          <button class="btn btn-sm" :class="on ? 'btn-ghost' : 'btn-primary'" type="button" :disabled="!camera.connected" @click="on ? stop() : start()">{{ on ? "Stop" : "Start" }}</button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.live { display: block; width: 100%; max-height: 360px; object-fit: fill; border-radius: 8px; background: #000; image-rendering: pixelated; }
.live-stats { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 0.75rem; margin: 1rem 0 0; }
.live-stats dt { color: var(--text-secondary); font-size: 1rem; }
.live-stats dd { margin: 0.375rem 0 0; overflow-wrap: anywhere; }
/* An 8-bit level, its scale and its share of full scale: 120 / 255 (47 %). */
.level { display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.3rem; }
.scale, .level :deep(.parenthesized::before), .level :deep(.parenthesized::after) { color: var(--text-secondary); }
</style>
