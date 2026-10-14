<script setup>
import { ref, computed, watch } from "vue";
import { useDeviceStore } from "../stores/device";
import { useTimingStore } from "../stores/timing";
import { useNotification } from "../composables/useNotification";
import { radioOf, isRadioConfig, RADIO_FREQ_MIN_KHZ, RADIO_FREQ_MAX_KHZ, RADIO_POWER_MIN_DBM, RADIO_POWER_MAX_DBM } from "../lib/protocol";

const notyf = useNotification();
const device = useDeviceStore();
const timing = useTimingStore();

const freq = ref(""); // MHz
const power = ref(""); // dBm at the chip
const lbt = ref(true);
const busy = ref(false);
const lastResult = ref(null);
const resultText = computed(() => ({
  ok: "Radio settings stored and applied (A R OK).",
  cfgapply: "Settings stored, but radio startup failed. The board will retry automatically.",
  cfgpending: "Settings stored. Application is pending until board initialization succeeds.",
  cfgfail: "Settings could not be stored on the board.",
  timeout: "No reply from the board; storage and application are unconfirmed.",
}[lastResult.value] || "The board rejected the settings."));

const board = computed(() => radioOf(device.identity));
const boardText = computed(() => {
  const b = board.value;
  if (!b) return device.connected ? "…" : "not connected";
  return `${(b.freqKhz / 1000).toFixed(3)} MHz · ${b.powerDbm} dBm · LBT ${b.lbt ? "on" : "off"}`;
});

const cfg = computed(() => {
  const f = freq.value.trim();
  const p = power.value.trim();
  if (!/^\d+(\.\d{1,3})?$/.test(f) || !/^-?\d+$/.test(p)) return null;
  const c = { freqKhz: Math.round(Number(f) * 1000), powerDbm: Number(p), lbt: lbt.value ? 1 : 0 };
  return isRadioConfig(c) ? c : null;
});
const onBoard = computed(() => {
  const b = board.value;
  const c = cfg.value;
  return !!b && !!c && b.freqKhz === c.freqKhz && b.powerDbm === c.powerDbm && b.lbt === c.lbt;
});

function load(b) {
  freq.value = (b.freqKhz / 1000).toFixed(3);
  power.value = String(b.powerDbm);
  lbt.value = b.lbt === 1;
  lastResult.value = null;
}

// The first board seen fills the form; later boards keep it, so one setting goes to every board.
watch(board, (b) => {
  if (b && !freq.value && !power.value) load(b);
}, { immediate: true });

async function apply() {
  if (!cfg.value || busy.value) return;
  busy.value = true;
  lastResult.value = null;
  try {
    const result = await device.configureRadio(cfg.value);
    lastResult.value = result;
    if (result === "ok") notyf.success(resultText.value);
    else notyf.error(resultText.value);
  } catch (e) {
    notyf.error(e.message);
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>📻 Radio</h3></div>
    <div class="card-body">
      <ol class="help">
        <li>Connect a board over USB, press Apply.</li>
        <li>Repeat for every master and sensor: boards on another frequency never hear each other.</li>
      </ol>

      <div class="fields">
        <label class="field">
          <span>Frequency</span>
          <input v-model="freq" class="form-input mono freq" type="text" inputmode="decimal" spellcheck="false" placeholder="922.900" />
          <span class="unit">MHz</span>
        </label>
        <label class="field">
          <span>Output power</span>
          <input v-model="power" class="form-input mono power" type="text" inputmode="numeric" spellcheck="false" placeholder="12" />
          <span class="unit">dBm at the chip</span>
        </label>
        <label class="field opt"><input v-model="lbt" type="checkbox" /> Listen before talk (LBT)</label>
      </div>
      <div class="meta">
        <span v-if="(freq || power) && !cfg" class="bad">
          Frequency {{ RADIO_FREQ_MIN_KHZ / 1000 }}–{{ RADIO_FREQ_MAX_KHZ / 1000 }} MHz (1 kHz steps), power {{ RADIO_POWER_MIN_DBM }} to {{ RADIO_POWER_MAX_DBM }} dBm
        </span>
      </div>
      <div class="board">
        Stored on this board: <span class="mono">{{ boardText }}</span>
        <span v-if="onBoard" class="ok">✓ stored values match</span>
      </div>

      <div class="btn-row">
        <button class="btn btn-primary btn-sm" type="button" :disabled="!board" @click="load(board)">Load from board</button>
        <button class="btn btn-success btn-sm send" type="button" :disabled="!cfg || !device.connected || busy || timing.armed" @click="apply">
          {{ busy ? "Applying…" : "Apply to connected board" }}
        </button>
      </div>
      <p v-if="timing.armed" class="hint">Stop the run to change the radio.</p>

      <div v-if="lastResult" class="result" :class="lastResult === 'ok' ? 'ok' : 'bad'">
        {{ resultText }}
      </div>
    </div>
  </div>
</template>

<style scoped>
.help {
  font-size: 0.85rem;
  color: var(--text-secondary);
  padding-left: 1.1rem;
  margin-bottom: 0.9rem;
}
.fields {
  display: flex;
  flex-wrap: wrap;
  gap: 0.75rem 1.25rem;
  align-items: center;
}
.field {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.9rem;
}
.freq {
  width: 7.5rem;
}
.power {
  width: 4rem;
}
.unit {
  color: var(--text-secondary);
  font-size: 0.85rem;
}
.meta {
  margin: 0.3rem 0 0.5rem;
  font-size: 0.8rem;
  min-height: 1rem;
}
.board {
  font-size: 0.85rem;
  color: var(--text-secondary);
  margin-bottom: 0.7rem;
}
.btn-row {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
}
.send {
  margin-left: auto;
}
.hint {
  margin-top: 0.5rem;
  font-size: 0.8rem;
  color: var(--text-secondary);
}
.result {
  margin-top: 0.75rem;
  padding: 0.5rem 0.75rem;
  border-radius: 8px;
  font-size: 0.85rem;
  font-weight: 600;
}
.result.ok {
  background: rgba(16, 185, 129, 0.15);
  color: var(--accent-success);
}
.result.bad {
  background: rgba(239, 68, 68, 0.15);
  color: var(--accent-danger);
}
.ok {
  color: var(--accent-success);
}
.bad {
  color: var(--accent-danger);
}
</style>
