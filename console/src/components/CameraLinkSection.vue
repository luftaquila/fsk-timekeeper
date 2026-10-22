<script setup>
/* The camera Pi on the tailnet (Settings → Camera Settings): its host, the connection to the
 * master behind it, and the camera's state. The Master card shows that master like a USB one. */
import { computed, ref, watch } from "vue";
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
const linkLabel = { online: "OK", degraded: "STALE", lost: "LOST" };
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

function bytes(n) {
  return Number.isFinite(n) ? `${(n / 1e9).toFixed(1)} GB` : "-";
}
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
          <div><dt>Camera</dt><dd>{{ camera.info?.camera?.model || "-" }} <span class="mono">{{ camera.info?.camera?.serial || "" }}</span>
            <span class="badge" :class="camera.info?.camera?.connected ? 'badge-success' : 'badge-danger'">{{ camera.info?.camera?.connected ? "Online" : "Offline" }}</span></dd></div>
          <div><dt>Camera Software</dt><dd class="mono">{{ camera.info?.app || "-" }}</dd></div>
          <div><dt>Camera Link</dt><dd><span class="badge" :class="report?.link_state === 'online' ? 'badge-success' : 'badge-warning'">{{ linkLabel[report?.link_state] || "NONE" }}</span></dd></div>
          <div><dt>Session</dt><dd class="mono">{{ camera.info?.session?.running ? camera.info.session.id : "Stopped" }}</dd></div>
          <div><dt>Line Rate</dt><dd class="mono">{{ camera.info?.session?.lineRateHz ? `${camera.info.session.lineRateHz.toLocaleString("en-US")} Hz` : "-" }}</dd></div>
          <div><dt>Detector Lag</dt><dd class="mono">{{ report?.latency_ms != null ? `${report.latency_ms} ms` : "-" }}</dd></div>
          <div><dt>Last Report</dt><dd class="mono">{{ report?.last_seen_at != null ? fmtAgeMs(Date.now() - report.last_seen_at) : "-" }}</dd></div>
          <div><dt>Lines Lost</dt><dd class="mono">{{ camera.info?.session?.lostLines ?? "-" }}</dd></div>
          <div><dt>Lost Line Records</dt><dd class="mono">{{ report?.fifo_drop ?? "-" }}</dd></div>
          <div><dt>Disk Free</dt><dd class="mono">{{ bytes(camera.info?.storage?.freeBytes) }}</dd></div>
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
.link-facts { display: grid; grid-template-columns: repeat(auto-fill, minmax(12rem, 1fr)); gap: 0.75rem; margin: 0; }
.link-facts dt { color: var(--text-secondary); font-size: 1rem; }
.link-facts dd { margin: 0.375rem 0 0; overflow-wrap: anywhere; }
.disconnect-btn:hover:not(:disabled), .disconnect-btn:focus-visible {
  color: #fff;
  background: var(--accent-danger);
}
.bad { color: var(--accent-danger); }
</style>
