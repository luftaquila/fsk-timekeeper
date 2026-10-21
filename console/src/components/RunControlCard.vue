<script setup>
import { computed } from "vue";
import { ROLE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { useTimingStore } from "../stores/timing";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";

defineProps({ note: { type: String, default: "" } });

const timing = useTimingStore();
const device = useDeviceStore();
const settings = useSettingsStore();

const missing = computed(() => timing.missingRoles(settings.state.mode));
const quality = computed(() => timing.qualityFor(settings.state.mode));
const canStart = computed(() => device.connected && !device.updateInProgress && !timing.armed && !timing.starting && !timing.classifying);
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🎛️ Control</h3></div>
    <div class="card-body">
      <div class="btn-group">
        <button class="btn btn-success" :disabled="!canStart" @click="timing.start(settings.state.mode, note)">{{ timing.starting ? "Starting…" : "Start" }}</button>
        <button class="btn btn-danger" :disabled="!timing.armed" @click="timing.stop()">Stop</button>
      </div>
      <div class="btn-group mt-1" aria-label="Classification">
        <button v-for="value in RESULT_CLASSIFICATIONS" :key="value" class="btn"
          :class="timing.run?.classification === value ? 'btn-danger' : 'btn-ghost'"
          :aria-pressed="timing.run?.classification === value"
          :disabled="timing.starting || timing.classifying"
          @click="timing.classify(timing.run?.classification === value ? null : value, { mode: settings.state.mode, note })">{{ value }}</button>
      </div>
      <button class="btn btn-ghost btn-block mt-1" :disabled="!timing.run || timing.armed || timing.classifying" @click="timing.reset()">Clear</button>
      <div class="readiness-status" role="status" aria-live="polite" aria-atomic="true">
        <div v-if="!device.connected || missing.length || !quality.ok"
          class="readiness-notice" :class="{ 'is-warning': device.connected && !missing.length }">
          <p v-if="!device.connected" class="readiness-message">Master not connected</p>
          <p v-else-if="missing.length" class="readiness-message">Unmapped role: {{ missing.map((r) => ROLE_LABEL[r] || r).join(", ") }}</p>
          <ul v-else class="readiness-reasons">
            <li v-for="(reason, i) in quality.reasons" :key="i">{{ reason.reason }}</li>
          </ul>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.readiness-notice {
  margin-top: 1rem;
  padding-top: 0.875rem;
  border-top: 1px solid var(--border-color);
  color: var(--accent-danger);
  font-size: 1rem;
  font-weight: 500;
  line-height: 1.5;
  overflow-wrap: anywhere;
}
.readiness-notice.is-warning {
  color: var(--accent-warning);
}
.readiness-reasons {
  list-style: none;
  min-width: 0;
}
.readiness-reasons li + li {
  margin-top: 0.625rem;
}
</style>
