<script setup>
import { useSettingsStore } from "../stores/settings";
import BridgeCard from "../components/BridgeCard.vue";
import MappingCard from "../components/MappingCard.vue";
import DiagnosticsCard from "../components/DiagnosticsCard.vue";
import ProvisionCard from "../components/ProvisionCard.vue";
import DfuCard from "../components/DfuCard.vue";
import SerialConsole from "../components/SerialConsole.vue";
import SimulatorCard from "../components/SimulatorCard.vue";

const settings = useSettingsStore();
const isDev = import.meta.env.DEV;

function digitsOnly(e) {
  e.target.value = e.target.value.replace(/\D/g, "");
}

function onDebounce(e) {
  settings.setDebounceMs(e.target.value);
  e.target.value = settings.state.debounceMs;
}
</script>

<template>
  <div class="settings">
    <div class="row">
      <BridgeCard />
      <div class="card">
        <div class="card-header"><h3>⏱️ Sensor debounce</h3></div>
        <div class="card-body">
          <div class="debounce-row">
            <input type="text" inputmode="numeric" class="form-input debounce-input" :value="settings.state.debounceMs" @input="digitsOnly" @change="onDebounce" />
            <span class="unit">ms</span>
          </div>
        </div>
      </div>
    </div>

    <MappingCard />
    <DiagnosticsCard />
    <ProvisionCard />
    <DfuCard />
    <SerialConsole />
    <SimulatorCard v-if="isDev" />
  </div>
</template>

<style scoped>
.settings {
  display: flex;
  flex-direction: column;
  gap: 1.5rem;
  max-width: 1100px;
  margin: 0 auto;
  width: 100%;
}
.row {
  display: grid;
  grid-template-columns: 1.4fr 1fr;
  gap: 1.5rem;
  align-items: stretch;
}
.debounce-row {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.debounce-input {
  max-width: 120px;
}
.unit {
  color: var(--text-tertiary);
  font-size: 0.9rem;
}
@media (max-width: 768px) {
  .row {
    grid-template-columns: 1fr;
  }
}
</style>
