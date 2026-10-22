<script setup>
/* Number reading on this console: the OCR confidence a Photo Laps reading needs to assign its
 * crossing by itself. Frozen into each run at Start. */
import { computed, ref, watch } from "vue";
import MetricValue from "./MetricValue.vue";
import { useSettingsStore } from "../stores/settings";
import { useActionResult } from "../composables/useNotification";
import { DEFAULT_OCR_MIN_CONF } from "../lib/constants";

const settings = useSettingsStore();
const value = ref(String(settings.state.ocrMinConf));
const lastResult = useActionResult();
const parsed = computed(() => (/^\d{1,3}$/.test(value.value.trim()) && Number(value.value) <= 100 ? Number(value.value) : null));
const saved = computed(() => settings.state.ocrMinConf);
watch(saved, (v) => { value.value = String(v); });
watch(value, () => { lastResult.value = null; });

function apply() {
  if (parsed.value == null || parsed.value === saved.value) return;
  settings.setOcrMinConf(parsed.value);
  lastResult.value = "ok";
}

function restoreDefault() {
  value.value = String(DEFAULT_OCR_MIN_CONF);
  apply();
}
</script>

<template>
  <section class="settings-section" aria-labelledby="camera-reading-title">
    <div class="section-heading">
      <h4 id="camera-reading-title">Number Reading</h4>
    </div>
    <div class="section-content">
      <div class="setting-grid" role="group" aria-label="Number Reading Settings">
        <div class="setting-labels" aria-hidden="true"><span>Setting</span><span>Saved Value</span><span>New Value</span></div>
        <div class="setting-row">
          <div class="setting-name"><label for="camera-ocr-conf">Minimum Confidence</label><small>0 ~ 100 %</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue :value="saved" unit="%" /></div>
          <div>
            <div class="value-input">
              <input id="camera-ocr-conf" v-model="value" class="form-input mono" type="text" inputmode="numeric" :aria-invalid="value !== '' && parsed == null" />
              <span class="unit">%</span>
            </div>
            <small v-if="value !== '' && parsed == null" class="bad field-error">Out of Range</small>
          </div>
        </div>
      </div>
      <p class="reading-help">Photo Laps readings below it stay Unassigned until the operator assigns them. Takes effect from the next run</p>
      <div class="section-actions">
        <span v-if="lastResult" class="action-result ok" role="status">Saved</span>
        <div class="apply-group">
          <span v-if="!lastResult && parsed != null && parsed !== saved" class="config-status unsaved">Unsaved Changes</span>
          <button class="btn btn-ghost btn-sm" type="button" :disabled="saved === DEFAULT_OCR_MIN_CONF" @click="restoreDefault">Default</button>
          <button class="btn btn-success btn-sm send" type="button" :disabled="parsed == null || parsed === saved" @click="apply">Apply</button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.setting-row, .setting-labels {
  display: grid;
  grid-template-columns: minmax(105px, 1fr) minmax(110px, 1fr) minmax(220px, 1.4fr);
  gap: 0.75rem;
  align-items: center;
}
.setting-labels { color: var(--text-primary); background: var(--bg-secondary); font-size: 1rem; font-weight: 600; padding: 0.65rem 0; }
.setting-row { min-height: 4.5rem; box-sizing: border-box; padding: 0.75rem 0; border-top: 1px solid var(--border-color); }
.setting-name { font-size: 1rem; }
.setting-name small { display: block; margin-top: 0.3rem; color: var(--text-tertiary); font-size: 1rem; font-weight: 400; }
.saved-value { font-size: 1rem; color: var(--text-primary); }
.mobile-label { display: none; }
.value-input { display: flex; align-items: center; gap: 0.5rem; }
.value-input input { min-width: 0; }
.field-error { display: block; margin-top: 0.35rem; font-size: 1rem; }
.unit { color: var(--text-secondary); font-size: 1rem; }
.reading-help { color: var(--text-secondary); font-size: 1rem; line-height: 1.6; margin: 0.5rem 0 0; }
.config-status { font-size: 1rem; }
.ok { color: var(--accent-success); }
.unsaved { color: var(--accent-warning); }
.bad { color: var(--accent-danger); }
@media (max-width: 600px) {
  .setting-labels { display: none; }
  .setting-row { grid-template-columns: minmax(0, 1fr); gap: 0.5rem; padding: 1rem 0; }
  .mobile-label { display: inline; }
  .setting-name { font-weight: 600; }
}
</style>
