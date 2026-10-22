<script setup>
/* One section of the settings kept on the camera Pi (Settings → Camera Settings): per field its
 * range, saved value and new value. The exposure's range follows the line rate entered in the same
 * section (else the saved one), and an entered line rate shows the rate it runs at. Apply sends the
 * section's fields, Default their defaults; a change restarts the camera session, so it waits for
 * the run to stop. */
import { computed, reactive, ref, watch } from "vue";
import MetricValue from "./MetricValue.vue";
import { useCameraStore } from "../stores/camera";
import { useTimingStore } from "../stores/timing";
import { useActionResult, notificationText } from "../composables/useNotification";
import { exposureMaxUs, lineRateAt } from "../lib/camera";

const props = defineProps({
  id: { type: String, required: true },
  title: { type: String, required: true },
  // [{ key, label, unit?, choices?: { value: label } }] of config.EDITABLE
  fields: { type: Array, required: true },
});
const RATE = "lineRateHz";
const EXPOSURE = "exposureUs";

const camera = useCameraStore();
const timing = useTimingStore();
const form = reactive({});
const busy = ref(false);
const lastResult = useActionResult();
const failure = ref("");
const resultText = computed(() => notificationText(lastResult.value === "ok" ? "Saved" : failure.value || "Settings Rejected"));

const saved = computed(() => camera.config?.values ?? null);
const limits = computed(() => camera.config?.limits ?? null);
const lineTiming = computed(() => camera.config?.lineTiming ?? null);
const rateField = computed(() => props.fields.find((f) => f.key === RATE) ?? null);
const blocked = computed(() => busy.value || !camera.connected || !saved.value || timing.armed);

const fmt = (n) => Number(n).toLocaleString("en-US", { maximumFractionDigits: 3 });
const fmtDown = (n) => fmt(Math.floor(n * 1000) / 1000); // a top never shown above itself
const choicesOf = (f) => limits.value?.[f.key]?.choices ?? Object.keys(f.choices);
// The field's range: the exposure's top is what the entered (else saved) line rate leaves it.
function rangeOf(f) {
  const l = limits.value?.[f.key];
  if (!l || f.key !== EXPOSURE || !lineTiming.value) return l;
  const rate = (rateField.value && valid(rateField.value) ? valueOf(rateField.value) : null) ?? saved.value?.[RATE];
  return rate == null ? l : { ...l, max: exposureMaxUs(rate, lineTiming.value) };
}
function hint(f) {
  const l = rangeOf(f);
  return !f.choices && l ? `${fmt(l.min)} ~ ${fmtDown(l.max)} ${f.unit}` : "";
}
// The rate an entered line rate runs at, when the master's whole-tick period moves it.
function runsAt(f) {
  if (f.key !== RATE || !lineTiming.value || !valid(f)) return null;
  const rate = Math.round(lineRateAt(valueOf(f), lineTiming.value));
  return rate === valueOf(f) ? null : rate;
}
const shown = (f, value) => (value == null ? "-" : f.choices ? f.choices[value] ?? value : fmt(value));
// The field's new value as sent: a choice, or a number.
function valueOf(f) {
  const value = String(form[f.key] ?? "").trim();
  if (f.choices) return choicesOf(f).includes(value) ? value : null;
  const plain = value.replaceAll(",", "");
  return /^\d+(?:\.\d+)?$/.test(plain) ? Number(plain) : null;
}
function valid(f) {
  const v = valueOf(f);
  const l = rangeOf(f);
  return v != null && !!l && (f.choices != null || (v >= l.min && v <= l.max));
}
const patch = computed(() => (props.fields.every(valid) ? Object.fromEntries(props.fields.map((f) => [f.key, valueOf(f)])) : null));
const onPi = computed(() => !!saved.value && !!patch.value && props.fields.every((f) => saved.value[f.key] === patch.value[f.key]));
const atDefault = computed(() => !!saved.value && !!limits.value && props.fields.every((f) => saved.value[f.key] === limits.value[f.key].default));

function load() {
  for (const f of props.fields) form[f.key] = saved.value ? String(saved.value[f.key]) : "";
  lastResult.value = null;
}
watch(saved, load, { immediate: true });
watch(() => props.fields.map((f) => form[f.key]), () => { lastResult.value = null; });

async function save(values) {
  busy.value = true;
  failure.value = "";
  lastResult.value = null;
  try {
    await camera.saveConfig(values);
    lastResult.value = "ok";
  } catch (error) {
    failure.value = error.message;
    lastResult.value = "error";
  } finally {
    busy.value = false;
  }
}

function apply() {
  if (patch.value && !onPi.value && !blocked.value) save(patch.value);
}

function restoreDefaults() {
  if (blocked.value || atDefault.value) return;
  if (!window.confirm(`Restore the default ${props.title} settings on the camera?`)) return;
  save(Object.fromEntries(props.fields.map((f) => [f.key, limits.value[f.key].default])));
}
</script>

<template>
  <section class="settings-section" :aria-labelledby="`${id}-title`">
    <div class="section-heading">
      <h4 :id="`${id}-title`">{{ title }}</h4>
    </div>
    <div class="section-content">
      <div class="setting-grid" role="group" :aria-label="`${title} Settings`">
        <div class="setting-labels" aria-hidden="true"><span>Setting</span><span>Saved Value</span><span>New Value</span></div>
        <div v-for="f in fields" :key="f.key" class="setting-row">
          <div class="setting-name"><label :for="`${id}-${f.key}`">{{ f.label }}</label><small>{{ hint(f) }}</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue :value="shown(f, saved?.[f.key])" :unit="f.unit" /></div>
          <div>
            <select v-if="f.choices" :id="`${id}-${f.key}`" v-model="form[f.key]" class="form-select" :disabled="!saved">
              <option v-for="c in choicesOf(f)" :key="c" :value="c">{{ f.choices[c] ?? c }}</option>
            </select>
            <div v-else class="value-input">
              <input :id="`${id}-${f.key}`" v-model="form[f.key]" class="form-input mono" type="text" inputmode="decimal" :disabled="!saved"
                :aria-invalid="form[f.key] !== '' && !valid(f)" />
              <span class="unit">{{ f.unit }}</span>
            </div>
            <small v-if="saved && form[f.key] !== '' && !valid(f)" class="bad field-error">Out of Range</small>
            <small v-else-if="runsAt(f) != null" class="field-note">Runs at {{ fmt(runsAt(f)) }} Hz</small>
          </div>
        </div>
      </div>
      <div class="section-actions">
        <span v-if="lastResult" class="action-result" :class="lastResult === 'ok' ? 'ok' : 'bad'" role="status">{{ resultText }}</span>
        <div class="apply-group">
          <span v-if="!lastResult && patch && saved && !onPi" class="config-status unsaved">Unsaved Changes</span>
          <button class="btn btn-ghost btn-sm" type="button" :disabled="blocked || atDefault" @click="restoreDefaults">Default</button>
          <button class="btn btn-success btn-sm send" type="button" :disabled="!patch || onPi || blocked" @click="apply">{{ busy ? "Applying…" : "Apply" }}</button>
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
.saved-value { font-size: 1rem; color: var(--text-primary); overflow-wrap: anywhere; }
.mobile-label { display: none; }
.value-input { display: flex; align-items: center; gap: 0.5rem; }
.value-input input { min-width: 0; }
.field-error, .field-note { display: block; margin-top: 0.35rem; font-size: 1rem; }
.field-note { color: var(--text-secondary); }
.unit { color: var(--text-secondary); font-size: 1rem; }
.config-status { font-size: 1rem; }
.ok { color: var(--accent-success); }
.unsaved { color: var(--accent-warning); }
.bad { color: var(--accent-danger); }
@media (max-width: 600px) {
  .setting-labels { display: none; }
  .setting-row { grid-template-columns: minmax(0, 1fr); gap: 0.5rem; padding: 1rem 0; }
  .setting-row:first-of-type { border-top: 0; }
  .mobile-label { display: inline; }
  .setting-name { font-weight: 600; }
}
</style>
