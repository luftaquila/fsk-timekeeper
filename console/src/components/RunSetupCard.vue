<script setup>
import { computed } from "vue";
import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
import { MODES, MODE_LABEL } from "../lib/constants";

defineProps({ modelValue: { type: String, default: "" } });
const emit = defineEmits(["update:modelValue"]);

const timing = useTimingStore();
const settings = useSettingsStore();

const mode = computed(() => timing.run?.mode ?? settings.state.mode);

function onMode(e) {
  settings.setMode(e.target.value);
  // A finished run belongs to the old mode: clear it from the page (History keeps it).
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
        <label for="lap-target" class="form-label">Auto-Stop After</label>
        <div class="lap-target-field">
          <input id="lap-target" type="text" inputmode="numeric" class="form-input lap-target" :value="settings.state.lapTarget ?? ''" placeholder="Off" aria-describedby="lap-target-unit" :disabled="timing.armed" @input="digitsOnly" @change="onTarget" />
          <span id="lap-target-unit" class="lap-target-unit">Laps</span>
        </div>
      </div>

    </div>
  </div>
</template>

<style scoped>
.lap-target-field {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.lap-target {
  flex: 1;
  min-width: 0;
}

.lap-target-unit {
  color: var(--text-secondary);
}
</style>
