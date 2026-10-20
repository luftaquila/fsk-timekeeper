<script setup>
import { computed, ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useFirmwareStore } from "../stores/firmware";
import { notificationText } from "../composables/useNotification";

const device = useDeviceStore();
const update = useFirmwareStore();
const dragging = ref(false);
const input = ref(null);
const locked = computed(() => update.busy || update.awaitingPort || update.phase === "checking");
const incompatible = computed(() => update.firmware && !update.firmware.compatible);
const stages = {
  checking: "Checking firmware…",
  preparing: "Preparing board…",
  "select-device": "Select Device",
  bootloader: "Connecting to bootloader…",
  "select-bootloader": "Select Bootloader",
  erasing: "Erasing firmware…",
  writing: "Writing firmware…",
  restarting: "Restarting board…",
  verifying: "Verifying firmware…",
  "select-app": "Verification Required",
  complete: "Firmware Upgraded",
  unverified: "Firmware Not Verified",
};
const status = computed(() => {
  if (update.phase === "error") return update.firmware ? "Upgrade Failed" : "Invalid Firmware File";
  if (incompatible.value) return "Incompatible Firmware";
  return stages[update.phase] || "";
});
const details = computed(() => {
  if (!device.supported) return "Use desktop Chrome or Edge to upgrade firmware over USB.";
  if (device.transportKind === "fake") return "Disconnect the simulator to upgrade a board over USB.";
  if (update.phase === "complete") return "";
  return notificationText(update.message);
});

function selected(event) {
  update.selectFile(event.target.files?.[0]);
  event.target.value = "";
}
function dropped(event) {
  dragging.value = false;
  if (!locked.value && event.dataTransfer.files.length === 1) update.selectFile(event.dataTransfer.files[0]);
}
</script>

<template>
  <section class="settings-section" aria-labelledby="firmware-upgrade-title">
    <div class="section-heading">
      <h4 id="firmware-upgrade-title">Firmware Upgrade</h4>
    </div>
    <div class="section-content">
      <div class="firmware-file" :class="{ dragging }" @dragover.prevent="dragging = !locked" @dragleave.prevent="dragging = false" @drop.prevent="dropped">
        <input ref="input" type="file" accept=".zip,application/zip" :disabled="locked" aria-label="Firmware ZIP" hidden @change="selected" />
        <div class="form-input filename" :class="{ empty: !update.filename }">{{ update.filename || 'No file selected' }}</div>
        <button class="btn btn-ghost btn-sm browse" type="button" :disabled="locked" aria-label="Browse Firmware ZIP" @click="input.click()">Browse</button>
      </div>
      <p class="firmware-help">Choose or drop a <a href="https://github.com/luftaquila/fsk-timekeeper/releases" target="_blank" rel="noopener noreferrer">release</a> ZIP file</p>
      <dl class="firmware-versions">
        <div><dt>Current Version</dt><dd class="mono">{{ device.identity?.fw || '-' }}</dd></div>
        <div><dt>Selected Version</dt><dd class="mono">{{ update.firmware?.metadata.version || '-' }}</dd></div>
      </dl>
      <div v-if="update.busy" class="firmware-progress">
        <div class="progress-heading">
          <span role="status">{{ stages[update.phase] }}</span>
          <span v-if="update.phase === 'writing'" class="progress-percent mono">{{ update.progress }}<span class="percent-unit">%</span></span>
        </div>
        <div class="progress-track" role="progressbar" :aria-label="stages[update.phase]" :aria-valuenow="update.phase === 'writing' ? update.progress : undefined" aria-valuemin="0" aria-valuemax="100">
          <div class="progress-fill" :class="{ indeterminate: update.phase !== 'writing' }" :style="update.phase === 'writing' ? { width: `${update.progress}%` } : undefined"></div>
        </div>
      </div>
      <p v-if="details" class="firmware-detail" :class="{ bad: update.phase === 'error' || incompatible }" role="status">{{ details }}</p>
      <div class="section-actions">
        <span v-if="status && !update.busy" class="action-result" :class="{ ok: update.phase === 'complete', bad: update.phase === 'error' || incompatible, pending: update.awaitingPort || update.phase === 'unverified' }" role="status">{{ status }}</span>
        <div class="apply-group">
          <button v-if="update.awaitingPort" class="btn btn-ghost btn-sm" type="button" @click="update.cancel">{{ update.phase === 'select-app' ? 'Skip Verification' : 'Cancel' }}</button>
          <button v-if="update.phase === 'select-device'" class="btn btn-primary btn-sm" type="button" @click="update.chooseDevice">Select Device</button>
          <button v-else-if="update.phase === 'select-bootloader'" class="btn btn-primary btn-sm" type="button" @click="update.chooseBootloader">Select Bootloader</button>
          <button v-else-if="update.phase === 'select-app'" class="btn btn-primary btn-sm" type="button" @click="update.chooseApp">Reconnect</button>
          <button v-else class="btn btn-sm send" :class="update.actionLabel === 'Downgrade' ? 'btn-warning' : 'btn-success'" type="button" :disabled="!update.canStart" @click="update.start">{{ update.busy ? 'Upgrading…' : update.actionLabel }}</button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.firmware-file { display: flex; align-items: center; gap: 0.5rem; border-radius: 8px; }
.firmware-file.dragging .filename { border-color: var(--border-focus); background: var(--bg-hover); }
.filename { min-width: 0; overflow-wrap: anywhere; }
.filename.empty { color: var(--text-tertiary); }
.browse { flex-shrink: 0; }
.firmware-help, .firmware-detail { color: var(--text-secondary); font-size: 1rem; line-height: 1.6; margin: 0.5rem 0 0; overflow-wrap: anywhere; }
.firmware-help a { color: var(--accent-primary); text-underline-offset: 0.15em; }
.firmware-versions { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0.75rem; margin: 1rem 0 0; }
.firmware-versions dt { color: var(--text-secondary); font-size: 1rem; }
.firmware-versions dd { margin: 0.375rem 0 0; overflow-wrap: anywhere; }
.firmware-progress { margin-top: 1.25rem; }
.progress-heading { display: flex; align-items: baseline; justify-content: space-between; gap: 1rem; margin-bottom: 0.625rem; color: var(--text-secondary); font-size: 1rem; }
.progress-percent { color: var(--text-primary); font-weight: 600; font-variant-numeric: tabular-nums; }
.percent-unit { margin-left: 0.2em; color: var(--text-secondary); font-weight: 400; }
.progress-track { height: 0.5rem; overflow: hidden; border-radius: 999px; background: var(--bg-hover); box-shadow: inset 0 0 0 1px var(--border-color); }
.progress-fill { height: 100%; border-radius: inherit; background: var(--accent-primary); transition: width 180ms ease-out; }
.progress-fill.indeterminate { width: 35%; animation: progress-sweep 1.6s ease-in-out infinite; }
@keyframes progress-sweep { from { transform: translateX(-100%); } to { transform: translateX(386%); } }
@media (prefers-reduced-motion: reduce) {
  .progress-fill { transition: none; }
  .progress-fill.indeterminate { animation: none; margin-left: 32.5%; }
}
.ok { color: var(--accent-success); }
.bad { color: var(--accent-danger); }
.pending { color: var(--accent-warning); }
</style>
