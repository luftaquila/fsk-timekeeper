<script setup>
import { ref, computed, watch } from "vue";
import MetricValue from "./MetricValue.vue";
import { useDeviceStore } from "../stores/device";
import { useTimingStore } from "../stores/timing";
import { useActionResult, notificationText } from "../composables/useNotification";
import { radioOf, isRadioConfig, RADIO_FREQ_MIN_KHZ, RADIO_FREQ_MAX_KHZ, RADIO_POWER_MIN_DBM, RADIO_POWER_MAX_DBM, RADIO_DEFAULT_CONFIG } from "../lib/protocol";

const device = useDeviceStore();
const timing = useTimingStore();

const frequency = ref("");
const power = ref(null);
const lbt = ref(1);
const frequencyRange = `${RADIO_FREQ_MIN_KHZ / 1000} ~ ${RADIO_FREQ_MAX_KHZ / 1000} MHz`;
const powerOptions = Array.from({ length: RADIO_POWER_MAX_DBM - RADIO_POWER_MIN_DBM + 1 }, (_, i) => RADIO_POWER_MIN_DBM + i);
const frequencyKhz = computed(() => {
  const value = frequency.value.trim();
  if (!/^\d+(?:\.\d{1,3})?$/.test(value)) return null;
  const khz = Math.round(Number(value) * 1000);
  return khz >= RADIO_FREQ_MIN_KHZ && khz <= RADIO_FREQ_MAX_KHZ ? khz : null;
});
const frequencyError = computed(() => {
  const value = frequency.value.trim();
  if (!/^\d+(?:\.\d+)?$/.test(value)) return "Invalid Frequency";
  if ((value.split(".")[1]?.length ?? 0) > 3) return "Maximum 3 Decimal Places";
  return "Frequency Out of Range";
});
const busy = ref(false);
const lastResult = useActionResult();
const failure = ref("");
const resultText = computed(() => notificationText({
  ok: "Saved",
  cfgapply: "Saved, Radio Start Failed",
  cfgpending: "Saved, Apply Pending",
  cfgfail: "Save Failed",
  timeout: "No Reply, Save Unconfirmed",
}[lastResult.value] || failure.value || "Settings Rejected"));

const board = computed(() => radioOf(device.identity));
const blocked = computed(() => busy.value || device.updateInProgress || !device.connected || device.keyReadState === "loading" || timing.armed);

const cfg = computed(() => {
  if (frequencyKhz.value == null || power.value == null) return null;
  const c = { freqKhz: frequencyKhz.value, powerDbm: power.value, lbt: lbt.value };
  return isRadioConfig(c) ? c : null;
});
const onBoard = computed(() => {
  const b = board.value;
  const c = cfg.value;
  return !!b && !!c && b.freqKhz === c.freqKhz && b.powerDbm === c.powerDbm && b.lbt === c.lbt;
});
const isDefault = (c) => c && Object.entries(RADIO_DEFAULT_CONFIG).every(([k, v]) => c[k] === v);
const defaultDisabled = computed(() => blocked.value || !board.value || (isDefault(board.value) && isDefault(cfg.value)));
watch([frequency, power, lbt], () => { lastResult.value = null; });

function load(b) {
  frequency.value = (b.freqKhz / 1000).toFixed(3);
  power.value = b.powerDbm;
  lbt.value = b.lbt;
  lastResult.value = null;
}

// Load each connection's settings without overwriting edits on periodic identity reports.
watch(() => device.identity?.devid, () => {
  if (board.value) load(board.value);
  else {
    frequency.value = "";
    power.value = null;
    lbt.value = 1;
    lastResult.value = null;
  }
}, { immediate: true });

async function apply() {
  if (!cfg.value || onBoard.value || blocked.value) return;
  if (!window.confirm("Apply these radio settings to the connected board?")) return;
  await save(cfg.value);
}

async function restoreDefaults() {
  if (defaultDisabled.value) return;
  if (!window.confirm("Restore default radio settings on the connected board?\n922.900 MHz · 12 dBm · Listen Before Talk On\nThese settings will be saved and applied immediately.")) return;
  load(RADIO_DEFAULT_CONFIG);
  if (!isDefault(board.value)) await save(RADIO_DEFAULT_CONFIG);
}

async function save(config) {
  busy.value = true;
  failure.value = "";
  lastResult.value = null;
  try {
    const result = await device.configureRadio(config);
    lastResult.value = result;
  } catch (e) {
    failure.value = e.message;
    lastResult.value = "error";
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <section class="settings-section" aria-labelledby="radio-title">
    <div class="section-heading">
      <h4 id="radio-title">Radio</h4>
    </div>
    <div class="section-content">
      <div class="radio-grid" role="group" aria-label="Radio Settings">
        <div class="radio-column-labels" aria-hidden="true"><span>Setting</span><span>Saved Value</span><span>New Value</span></div>
        <div class="radio-row">
          <div class="setting-name"><label for="radio-frequency">Frequency</label><small id="frequency-range">{{ frequencyRange }}</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue class="frequency-current" :value="board ? (board.freqKhz / 1000).toFixed(3) : '-'" unit="MHz" /></div>
          <div>
            <div class="frequency-input">
              <input id="radio-frequency" v-model="frequency" class="form-input mono freq" type="text" inputmode="decimal" aria-label="Frequency" aria-describedby="frequency-range frequency-error" :aria-invalid="frequency !== '' && frequencyKhz == null" />
              <span class="unit">MHz</span>
            </div>
            <small v-if="frequency !== '' && frequencyKhz == null" id="frequency-error" class="bad">{{ frequencyError }}</small>
          </div>
        </div>
        <div class="radio-row">
          <div class="setting-name"><label for="radio-power">Output Power</label><small id="power-range">{{ RADIO_POWER_MIN_DBM }} ~ {{ RADIO_POWER_MAX_DBM }} dBm</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue class="power-current" :value="board?.powerDbm" unit="dBm" /></div>
          <div class="power-input">
            <select id="radio-power" aria-describedby="power-range power-unit" v-model="power" class="form-select power">
              <option :value="null" disabled>Select</option>
              <option v-for="dbm in powerOptions" :key="dbm" :value="dbm">{{ dbm }}</option>
            </select>
            <span id="power-unit" class="unit">dBm</span>
          </div>
        </div>
        <div class="radio-row">
          <div class="setting-name"><label for="radio-lbt">Listen Before Talk</label><small>5 ms / Scan</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue class="lbt-current" :value="board ? (board.lbt ? 'On' : 'Off') : '-'" /></div>
          <select id="radio-lbt" v-model="lbt" class="form-select lbt">
            <option :value="1">On</option>
            <option :value="0">Off</option>
          </select>
        </div>
        <div class="radio-row fixed-radio-row">
          <div class="setting-name">Spreading Factor<small>SF5 ~ SF12</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue :value="device.identity ? `SF${device.identity.sf}` : '-'" /></div>
          <div class="fixed-value">Fixed</div>
        </div>
        <div class="radio-row fixed-radio-row">
          <div class="setting-name">Bandwidth<small>7.81 ~ 500 kHz</small></div>
          <div class="saved-value"><span class="mobile-label">Saved: </span><MetricValue :value="device.identity?.bw" unit="kHz" /></div>
          <div class="fixed-value">Fixed</div>
        </div>
      </div>
      <div class="section-actions radio-actions">
        <span v-if="lastResult" class="action-result" :class="lastResult === 'ok' ? 'ok' : 'bad'" role="status">{{ resultText }}</span>
        <div class="apply-group">
          <div v-if="!lastResult && board && cfg && !onBoard" class="config-status" aria-live="polite">
            <span class="unsaved">Unsaved Changes</span>
          </div>
          <button class="btn btn-ghost btn-sm restore-defaults" type="button" :disabled="defaultDisabled" @click="restoreDefaults">Default</button>
          <button class="btn btn-success btn-sm send" type="button" :disabled="!cfg || onBoard || blocked" @click="apply">
            {{ busy ? "Applying…" : "Apply" }}
          </button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.radio-row, .radio-column-labels {
  display: grid;
  grid-template-columns: minmax(105px, 1fr) minmax(110px, 1fr) minmax(220px, 1.4fr);
  gap: 0.75rem;
  align-items: center;
}
.radio-column-labels { color: var(--text-primary); background: var(--bg-secondary); font-size: 1rem; font-weight: 600; padding: 0.65rem 0; }
.fixed-value { font-size: 1rem; color: var(--text-secondary); }
.radio-row { min-height: 4.5rem; box-sizing: border-box; padding: 0.75rem 0; border-top: 1px solid var(--border-color); }
.setting-name { font-size: 1rem; }
.saved-value { font-size: 1rem; color: var(--text-primary); overflow-wrap: anywhere; }
.mobile-label { display: none; }
.setting-name small { display: block; margin-top: 0.3rem; color: var(--text-tertiary); font-size: 1rem; font-weight: 400; }
.frequency-input, .power-input { display: flex; align-items: center; gap: 0.5rem; }
.freq, .power { min-width: 0; }
#frequency-error { display: block; margin-top: 0.35rem; font-size: 1rem; }
.unit { color: var(--text-secondary); font-size: 1rem; }
.config-status { min-width: 0; text-align: right; color: var(--text-secondary); font-size: 1rem; }
.radio-actions { flex-wrap: wrap; }
.ok { color: var(--accent-success); }
.unsaved { color: var(--accent-warning); }
.bad { color: var(--accent-danger); }
@media (max-width: 600px) {
  .config-status { flex-basis: 100%; }
  .radio-column-labels { display: none; }
  .radio-row { grid-template-columns: minmax(0, 1fr); gap: 0.5rem; padding: 1rem 0; }
  .radio-row:first-of-type { border-top: 0; }
  .mobile-label { display: inline; }
  .setting-name { font-weight: 600; }
}
</style>
