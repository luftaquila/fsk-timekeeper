<script setup>
import { computed } from "vue";
import { ROLE_LABEL } from "../lib/constants";
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
const canStart = computed(() => device.connected && !timing.armed && !timing.starting);
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🎛️ Control</h3></div>
    <div class="card-body">
      <div class="btn-group">
        <button class="btn btn-success" :disabled="!canStart" @click="timing.start(settings.state.mode, note)">{{ timing.starting ? "Starting…" : "Start" }}</button>
        <button class="btn btn-danger" :disabled="!timing.armed" @click="timing.stop()">Stop</button>
      </div>
      <button class="btn btn-ghost btn-block mt-1" :disabled="!timing.run || timing.armed" @click="timing.reset()">Clear</button>
      <p v-if="!device.connected" class="note bad">Master not connected.</p>
      <p v-else-if="missing.length" class="note bad">Unmapped role: {{ missing.map((r) => ROLE_LABEL[r] || r).join(", ") }} — see Settings.</p>
      <p v-else-if="!quality.ok" class="note warn" :title="quality.reasons.map((r) => r.reason).join('\n')">
        Not ready: {{ quality.reasons[0].reason }}<span v-if="quality.reasons.length > 1"> (+{{ quality.reasons.length - 1 }})</span>
      </p>
      <p v-else class="note ok">Ready.</p>
    </div>
  </div>
</template>

<style scoped>
.note {
  margin-top: 0.75rem;
  font-size: 0.8rem;
}
.note.ok {
  color: var(--accent-success);
}
.note.warn {
  color: var(--accent-warning);
}
.note.bad {
  color: var(--accent-danger);
}
</style>
