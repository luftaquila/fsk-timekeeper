<script setup>
import BridgeCard from "../components/BridgeCard.vue";
import MappingCard from "../components/MappingCard.vue";
import ProvisionCard from "../components/ProvisionCard.vue";
import RadioCard from "../components/RadioCard.vue";
import DfuCard from "../components/DfuCard.vue";
import SerialConsole from "../components/SerialConsole.vue";
import DeviceLogCard from "../components/DeviceLogCard.vue";
import SimulatorCard from "../components/SimulatorCard.vue";

const isDev = import.meta.env.DEV;
</script>

<template>
  <div class="settings">
    <BridgeCard />
    <MappingCard />
    <details class="card board-settings">
      <summary class="card-header settings-toggle">
        <h3><span class="heading-icon" aria-hidden="true">⚙️</span><span>Device Settings</span></h3>
        <span class="disclosure" aria-hidden="true">›</span>
      </summary>
      <p class="settings-notice">All masters and sensors must use the same radio settings, key, and firmware.</p>
      <div class="settings-sections">
        <RadioCard />
        <ProvisionCard />
        <DfuCard />
      </div>
    </details>
    <DeviceLogCard />
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
.settings :deep(.card-header h3) { gap: 0.75rem; }
.settings :deep(.heading-icon) { flex-shrink: 0; }
.settings-toggle { display: flex; align-items: center; justify-content: space-between; gap: 1rem; cursor: pointer; user-select: none; list-style: none; }
.settings-toggle::-webkit-details-marker { display: none; }
.settings-toggle:focus-visible { outline: 2px solid var(--accent-primary); outline-offset: -3px; }
.board-settings:not([open]) > .settings-toggle { border-bottom: 0; }
.board-settings[open] .disclosure { transform: rotate(90deg); }
.settings-notice { padding: 1rem 1.25rem 0; color: var(--text-secondary); font-size: 0.85rem; }
.settings-sections {
  padding: 0 1.25rem;
}
.settings-sections :deep(.settings-section) {
  display: grid;
  grid-template-columns: 165px minmax(0, 1fr);
  gap: 1.5rem;
  padding: 1.5rem 0;
}
.settings-sections :deep(.settings-section + .settings-section) {
  border-top: 1px solid var(--border-color);
}
.settings-sections :deep(.section-heading h4) {
  font-size: 0.95rem;
  margin: 0 0 0.5rem;
}
.settings-sections :deep(.section-content) { min-width: 0; }
.settings-sections :deep(.help) {
  font-size: 0.85rem;
  line-height: 1.6;
  color: var(--text-secondary);
  padding-left: 1.1rem;
  margin: 0;
}
.settings-sections :deep(.section-actions) {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
  margin-top: 1rem;
}
.settings-sections :deep(.radio-actions) { flex-wrap: nowrap; }
.settings-sections :deep(.action-result) { min-width: 0; font-size: 0.8rem; font-weight: 600; }
.settings-sections :deep(.apply-group) {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 0.75rem;
  margin-left: auto;
  min-width: 0;
  max-width: 100%;
}
@media (max-width: 900px) {
  .settings-sections :deep(.settings-section) {
    grid-template-columns: minmax(0, 1fr);
    gap: 1rem;
  }
}
</style>
