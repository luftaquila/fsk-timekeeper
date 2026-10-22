<script setup>
/* The camera Pi on the tailnet (Settings → Camera Settings): its host, the connection to the
 * master behind it, and the camera's state. The Master card shows that master like a USB one. */
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import MetricValue from "./MetricValue.vue";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";
import { useCameraStore } from "../stores/camera";
import { useTimingStore } from "../stores/timing";
import { useNotification } from "../composables/useNotification";
import { fmtAgeMs } from "../lib/format";

const device = useDeviceStore();
const settings = useSettingsStore();
const camera = useCameraStore();
const timing = useTimingStore();
const notyf = useNotification();

const host = ref(settings.state.cameraHost);
watch(() => settings.state.cameraHost, (value) => { host.value = value; });
const otherMaster = computed(() => device.connected && device.transportKind !== "pi");
const report = computed(() => (device.cameraNode ? device.telemetry[device.cameraNode] : null));
// Last Report counts every second between the master's reports.
const now = ref(Date.now());
let clock;
onMounted(() => { clock = setInterval(() => (now.value = Date.now()), 1000); });
onUnmounted(() => clearInterval(clock));
const problem = computed(() => {
  if (camera.connected) return camera.info?.session?.error || null;
  if (otherMaster.value) return "A master is connected over USB: disconnect it first";
  if (camera.infoError) return camera.infoError;
  return camera.info && !camera.contract.ok ? camera.contract.reason : null;
});

function saveHost() {
  try {
    settings.setCameraHost(host.value);
    host.value = settings.state.cameraHost;
    return true;
  } catch (error) {
    notyf.error(error.message);
    return false;
  }
}

async function connect() {
  if (!saveHost()) return;
  try {
    await camera.connect();
  } catch (error) {
    notyf.error(error.message || "Camera not reachable");
  }
}

function disconnect() {
  if (window.confirm("Disconnect from the camera?")) device.disconnect();
}

// Figures as the master and sensor diagnostics show them: number and unit apart, no unit without
// a number.
const metric = (value, unit = "") => (value == null ? { value: "-", unit: "" } : { value, unit });
const facts = computed(() => {
  const session = camera.info?.session;
  const free = camera.info?.storage?.freeBytes;
  const age = report.value?.last_seen_at != null ? fmtAgeMs(Math.max(0, now.value - report.value.last_seen_at)) : null;
  return {
    lastReport: age && /^\d+[sm]$/.test(age) ? metric(age.slice(0, -1), age.slice(-1)) : metric(age),
    diskFree: metric(Number.isFinite(free) ? (free / 1e9).toFixed(1) : null, "GB"),
    lineRate: metric(session?.lineRateHz ? session.lineRateHz.toLocaleString("en-US") : null, "Hz"),
    // Lines come in frames of 1000: thousands only.
    captured: metric(Number.isFinite(session?.capturedLines) ? Math.floor(session.capturedLines / 1000).toLocaleString("en-US") : null, "k"),
    lost: metric(session?.lostLines ?? null),
    delay: metric(report.value?.latency_ms ?? null, "ms"),
  };
});
</script>

<template>
  <section class="settings-section" aria-labelledby="camera-link-title">
    <div class="section-heading">
      <h4 id="camera-link-title">Camera Pi</h4>
    </div>
    <div class="section-content">
      <template v-if="!camera.connected">
        <div class="host-field">
          <input id="camera-host" v-model="host" class="form-input mono" placeholder="camera.tailnet-name.ts.net" aria-label="Camera Host"
            spellcheck="false" autocapitalize="off" @change="saveHost" />
        </div>
        <p class="link-help">The Pi's tailnet name. This computer must be on the same tailnet</p>
      </template>
      <template v-else>
        <dl class="link-facts">
          <div><dt>Camera</dt><dd>{{ camera.info?.camera?.model || "-" }}</dd></div>
          <div><dt>Camera Software</dt><dd>{{ camera.info?.app || "-" }}</dd></div>
          <div><dt>Last Report</dt><dd><MetricValue v-bind="facts.lastReport" /></dd></div>
          <div><dt>Disk Free</dt><dd><MetricValue v-bind="facts.diskFree" /></dd></div>
          <div><dt>Line Rate</dt><dd><MetricValue v-bind="facts.lineRate" /></dd></div>
          <div><dt>Lines Captured</dt><dd><MetricValue v-bind="facts.captured" /></dd></div>
          <div><dt>Lines Lost</dt><dd><MetricValue v-bind="facts.lost" /></dd></div>
          <div><dt>Detector Delay</dt><dd><MetricValue v-bind="facts.delay" /></dd></div>
        </dl>
      </template>
      <div class="section-actions">
        <span v-if="problem" class="action-result bad" role="status">{{ problem }}</span>
        <div class="apply-group">
          <button v-if="!camera.connected" class="btn btn-success btn-sm" type="button" :disabled="otherMaster || device.connecting || !host" @click="connect">Connect</button>
          <button v-else class="btn btn-ghost btn-sm disconnect-btn" type="button" :disabled="timing.armed" @click="disconnect">Disconnect</button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.host-field { display: flex; align-items: center; gap: 0.5rem; }
.host-field input { min-width: 0; }
.link-help { color: var(--text-secondary); font-size: 1rem; line-height: 1.6; margin: 0.5rem 0 0; }
.link-facts { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 0.75rem; margin: 0; }
.link-facts dt { color: var(--text-secondary); font-size: 1rem; }
/* As the master and sensor diagnostics (NodeDiagnostics) show their values. */
.link-facts dd { margin: 0.375rem 0 0; color: color-mix(in srgb, var(--text-primary) 30%, var(--text-secondary)); font-family: var(--font-mono);
  font-size: 1.125rem; font-weight: 600; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
.link-facts dd :deep(.metric-number) { font-size: 1.125rem; }
.link-facts dd :deep(.metric-unit) { color: var(--text-secondary); }
.disconnect-btn:hover:not(:disabled), .disconnect-btn:focus-visible {
  color: #fff;
  background: var(--accent-danger);
}
.bad { color: var(--accent-danger); }
@media (max-width: 600px) {
  .link-facts { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
</style>
