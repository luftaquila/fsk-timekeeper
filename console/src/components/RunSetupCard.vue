<script setup>
import { computed } from "vue";
import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
import { MODES, MODE_LABEL } from "../lib/constants";
import { fmtPpm } from "../lib/format";

defineProps({ modelValue: { type: String, default: "" } });
const emit = defineEmits(["update:modelValue"]);

const timing = useTimingStore();
const settings = useSettingsStore();

const mode = computed(() => timing.run?.mode ?? settings.state.mode);

function onMode(e) {
  settings.setMode(e.target.value);
  // A finished run belongs to the old mode; clear it so the page shows the new one (History keeps it).
  if (timing.run && !timing.armed) timing.reset();
}

// Digits only in every browser (type=number still lets letters through in some).
function digitsOnly(e) {
  e.target.value = e.target.value.replace(/\D/g, "");
}

function onTarget(e) {
  settings.setLapTarget(e.target.value);
  e.target.value = settings.state.lapTarget ?? "";
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>📝 Run</h3></div>
    <div class="card-body">
      <div class="form-group">
        <label class="form-label">Mode</label>
        <select class="form-select mode" :value="mode" :disabled="timing.armed" @change="onMode">
          <option v-for="m in MODES" :key="m" :value="m">{{ MODE_LABEL[m] }}</option>
        </select>
      </div>

      <div class="form-group">
        <label class="form-label">Note</label>
        <input :value="modelValue" class="form-input note" :disabled="timing.armed" @input="emit('update:modelValue', $event.target.value)" />
      </div>

      <div v-if="mode === 'laps'" class="form-group">
        <label class="form-label">Auto-stop after N laps</label>
        <input type="text" inputmode="numeric" class="form-input lap-target" :value="settings.state.lapTarget ?? ''" placeholder="off" :disabled="timing.armed" @input="digitsOnly" @change="onTarget" />
      </div>

      <div v-if="timing.run" class="run-meta">
        <span>{{ timing.run.note || "(no note)" }}<span v-if="timing.run.calib" class="ppm">{{ fmtPpm(timing.run.calib.ppb) }}</span></span>
        <span class="badge" :class="timing.run.verification === 'verified' ? 'badge-success' : timing.run.verification === 'invalid' ? 'badge-danger' : 'badge-warning'">{{ timing.run.verification }}</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.ppm {
  margin-left: 0.5rem;
  color: var(--text-tertiary);
  font-family: var(--font-mono);
}
.run-meta {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 0.5rem;
  margin-top: 0.25rem;
  font-size: 0.8rem;
  color: var(--text-secondary);
}
</style>
