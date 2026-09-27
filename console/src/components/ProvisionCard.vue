<script setup>
import { ref, computed } from "vue";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";
import { isHexKey } from "../lib/protocol";

const notyf = useNotification();
const device = useDeviceStore();

// Kept in memory only: the console never needs the key to talk to the master.
const key = ref("");
const busy = ref(false);
const lastResult = ref(null);

const valid = computed(() => isHexKey(key.value.trim()));

function generate() {
  const buf = new Uint8Array(32);
  crypto.getRandomValues(buf);
  key.value = Array.from(buf, (b) => b.toString(16).padStart(2, "0")).join("");
  lastResult.value = null;
}

async function send() {
  if (!valid.value || busy.value) return;
  busy.value = true;
  lastResult.value = null;
  try {
    const result = await device.provisionKey(key.value.trim());
    lastResult.value = result;
    if (result === "ok") notyf.success("Key stored on the board (A K OK)");
    else if (result === "keyfail") notyf.error("The board rejected the key (X keyfail)");
    else notyf.error("No reply from the board");
  } catch (e) {
    notyf.error(e.message);
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🔑 Radio key</h3></div>
    <div class="card-body">
      <ol class="help">
        <li>Generate a key.</li>
        <li>Connect a board over USB, press Send.</li>
        <li>Repeat for every master and sensor.</li>
        <li>Adding a board later: repeat with a new key on every board.</li>
      </ol>

      <input v-model.trim="key" class="form-input mono key-input" type="text" spellcheck="false" autocomplete="off" placeholder="64 hex characters" />
      <div class="key-meta">
        <span v-if="key && !valid" class="bad">Not a valid key (64 hex characters)</span>
      </div>

      <div class="btn-row">
        <button class="btn btn-primary btn-sm" type="button" @click="generate">Generate</button>
        <button class="btn btn-success btn-sm send" type="button" :disabled="!valid || !device.connected || busy" @click="send">
          {{ busy ? "Sending…" : "Send to connected board" }}
        </button>
      </div>

      <div v-if="lastResult" class="result" :class="lastResult === 'ok' ? 'ok' : 'bad'">
        {{ lastResult === "ok" ? "Board provisioned." : lastResult === "keyfail" ? "Board rejected the key." : "No reply from the board." }}
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
.key-input {
  letter-spacing: 0.02em;
}
.key-meta {
  margin: 0.3rem 0 0.7rem;
  font-size: 0.8rem;
  min-height: 1rem;
}
.btn-row {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
}
.send {
  margin-left: auto;
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
.bad {
  color: var(--accent-danger);
}
</style>
