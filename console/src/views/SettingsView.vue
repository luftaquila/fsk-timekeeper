<script setup>
import BridgeCard from "../components/BridgeCard.vue";
import MappingCard from "../components/MappingCard.vue";
import ProvisionCard from "../components/ProvisionCard.vue";
import RadioCard from "../components/RadioCard.vue";
import DfuCard from "../components/DfuCard.vue";
import SerialConsole from "../components/SerialConsole.vue";
import DeviceLogCard from "../components/DeviceLogCard.vue";
import SimulatorCard from "../components/SimulatorCard.vue";
import CameraConnectionLight from "../components/CameraConnectionLight.vue";
import CameraLinkSection from "../components/CameraLinkSection.vue";
import CameraConfigSection from "../components/CameraConfigSection.vue";
import CameraLiveSection from "../components/CameraLiveSection.vue";
import CameraReadingSection from "../components/CameraReadingSection.vue";

import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
const timing = useTimingStore();
const settings = useSettingsStore();

// Settings kept on the camera Pi (config.EDITABLE).
const CAPTURE_FIELDS = [
  { key: "lineRateHz", label: "Line Rate", unit: "Hz" },
  { key: "exposureUs", label: "Exposure", unit: "µs" },
  { key: "gainDb", label: "Gain", unit: "dB" },
  { key: "travel", label: "Travel Direction", note: "As seen from the camera", choices: { left: "Right to Left", right: "Left to Right" } },
];
const DETECTION_FIELDS = [
  { key: "bandTop", label: "Band Top", unit: "px" },
  { key: "bandBottom", label: "Band Bottom", unit: "px" },
  { key: "threshold", label: "Threshold", unit: "%" },
];
const CLIP_FIELDS = [
  { key: "clipBeforeMs", label: "Margin", note: "Before the front and after the car", unit: "ms" },
  { key: "clipMaxMs", label: "Maximum Length", note: "From the front", unit: "ms" },
  { key: "clipQuotaGb", label: "Storage Limit", note: "Oldest sessions go first", unit: "GB" },
];

const isDev = import.meta.env.DEV;
</script>

<template>
  <div class="settings">
    <BridgeCard />
    <MappingCard />
    <details class="card board-settings">
      <summary class="card-header settings-toggle">
        <h3><span class="heading-icon" aria-hidden="true">⚙️</span><span>Device Settings</span></h3>
        <svg class="disclosure" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 6 6 6-6 6" /></svg>
      </summary>
      <p class="settings-notice">All masters and sensors must use the same radio settings, key, and firmware</p>
      <p v-if="timing.armed" class="settings-notice">Stop the run to change device settings</p>
      <div class="settings-sections">
        <RadioCard />
        <ProvisionCard />
        <DfuCard />
      </div>
    </details>
    <details class="card board-settings" :open="!!settings.state.cameraHost">
      <summary class="card-header settings-toggle">
        <h3><span class="heading-icon" aria-hidden="true">📷</span><span>Camera Settings</span><CameraConnectionLight /></h3>
        <svg class="disclosure" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 6 6 6-6 6" /></svg>
      </summary>
      <p class="settings-notice">Capture, detection and clip settings are kept on the camera Pi and restart its camera session</p>
      <p v-if="timing.armed" class="settings-notice">Stop the run to change camera settings</p>
      <div class="settings-sections">
        <CameraLinkSection />
        <CameraLiveSection />
        <CameraConfigSection id="camera-capture" title="Capture" :fields="CAPTURE_FIELDS" />
        <CameraConfigSection id="camera-detection" title="Detection" :fields="DETECTION_FIELDS" />
        <CameraConfigSection id="camera-clips" title="Clips" :fields="CLIP_FIELDS" />
        <CameraReadingSection />
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
.settings :deep(.btn) {
  padding: 0.375rem 0.75rem;
  font-size: 0.9375rem;
}
.settings-toggle { display: flex; align-items: center; justify-content: space-between; gap: 1rem; cursor: pointer; user-select: none; list-style: none; }
.settings-toggle::-webkit-details-marker { display: none; }
.settings-toggle:focus-visible { outline: 2px solid var(--accent-primary); outline-offset: -3px; }
.board-settings:not([open]) > .settings-toggle { border-bottom: 0; }
.board-settings[open] .disclosure { transform: rotate(90deg); }
.settings-notice { padding: 1rem 1.25rem 0; color: var(--text-secondary); font-size: 1rem; }
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
  font-size: 1.125rem;
  margin: 0 0 0.5rem;
}
.settings-sections :deep(.section-content) { min-width: 0; }
.settings-sections :deep(.help) {
  font-size: 1rem;
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

.settings-sections :deep(.action-result) { min-width: 0; overflow-wrap: anywhere; font-size: 1rem; font-weight: 600; }
.settings-sections :deep(.section-actions .btn) {
  line-height: 1.25;
  white-space: nowrap;
  flex-shrink: 0;
}
.settings-sections :deep(.send:disabled) {
  color: var(--text-secondary);
  background: var(--bg-hover);
  box-shadow: inset 0 0 0 1px var(--border-color);
  opacity: 1;
}
.settings-sections :deep(.apply-group) {
  display: flex;
  flex-wrap: wrap;
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
