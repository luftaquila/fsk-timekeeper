<script setup>
import { ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useTimingStore } from "../stores/timing";
import { useNotification } from "../composables/useNotification";

const notyf = useNotification();
const device = useDeviceStore();
const timing = useTimingStore();
const busy = ref(false);
const confirming = ref(false);

async function enter() {
  if (busy.value) return;
  if (!confirming.value) {
    confirming.value = true;
    setTimeout(() => (confirming.value = false), 5000);
    return;
  }
  confirming.value = false;
  busy.value = true;
  try {
    await device.enterBootloader();
  } catch (e) {
    notyf.error(e.message);
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>⬆️ Firmware update</h3></div>
    <div class="card-body">
      <ol class="help">
        <li>Enter bootloader (button below), or double-tap <code>RST</code> on a new board.</li>
        <li>Run <code>flash.bat</code> (Windows) or <code>./flash.sh</code> (Linux / macOS) from the release zip.</li>
      </ol>
      <button class="btn btn-sm" :class="confirming ? 'btn-danger' : 'btn-warning'" :disabled="!device.connected || busy || timing.armed" @click="enter">
        {{ busy ? "Resetting…" : confirming ? "Click again to confirm" : "Enter bootloader" }}
      </button>
      <span v-if="timing.armed" class="muted">Stop the running measurement first.</span>
    </div>
  </div>
</template>

<style scoped>
.help {
  font-size: 0.85rem;
  color: var(--text-secondary);
  padding-left: 1.1rem;
  margin-bottom: 1rem;
}
.help li {
  margin-bottom: 0.2rem;
}
code {
  font-family: var(--font-mono);
  font-size: 0.8rem;
  background: var(--bg-hover);
  padding: 0.05rem 0.3rem;
  border-radius: 4px;
}
.muted {
  margin-left: 0.75rem;
  font-size: 0.8rem;
  color: var(--text-tertiary);
}
</style>
