<script setup>
import { computed } from "vue";
import { ROLE_LABEL, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { useTimingStore } from "../stores/timing";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";

// note = the run note input
defineProps({ note: { type: String, default: "" } });

const timing = useTimingStore();
const device = useDeviceStore();
const settings = useSettingsStore();

const missing = computed(() => timing.missingRoles(settings.state.mode));
const quality = computed(() => timing.qualityFor(settings.state.mode));
const canStart = computed(() => device.connected && !timing.armed && !timing.starting && !timing.classifying);
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
          @click="timing.classify(timing.run?.classification === value ? null : value, settings.state.mode, note)">{{ value }}</button>
      </div>
      <button class="btn btn-ghost btn-block mt-1" :disabled="!timing.run || timing.armed || timing.classifying" @click="timing.reset()">Clear</button>
      <p v-if="!device.connected" class="note bad">Master not connected.</p>
      <p v-else-if="missing.length" class="note bad">Unmapped Role: {{ missing.map((r) => ROLE_LABEL[r] || r).join(", ") }}</p>
      <ul v-else-if="!quality.ok" class="note warn readiness-reasons">
        <li v-for="(reason, i) in quality.reasons" :key="i">{{ reason.reason }}</li>
      </ul>
    </div>
  </div>
</template>

<style scoped>
.readiness-reasons { padding-left: 1.1rem; }
.note { overflow-wrap: anywhere; }
.note {
  margin-top: 0.75rem;
  font-size: 0.8rem;
}
.note.warn {
  color: var(--accent-warning);
}
.note.bad {
  color: var(--accent-danger);
}
</style>
