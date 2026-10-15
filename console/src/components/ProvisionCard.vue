<script setup>
import { ref, computed, watch } from "vue";
import { useDeviceStore } from "../stores/device";
import { useTimingStore } from "../stores/timing";
import { useActionResult, notificationText } from "../composables/useNotification";
import { isHexKey } from "../lib/protocol";

const device = useDeviceStore();
const timing = useTimingStore();

// Loaded over USB and kept only in memory; never saved in browser storage.
const key = ref("");
const visible = ref(false);
const busy = ref(false);
const lastResult = useActionResult();
const failure = ref("");
const resultText = computed(() => notificationText({ ok: "Saved", keyfail: "Key Save Failed", timeout: "No Reply, Save Unconfirmed" }[lastResult.value] || failure.value || "Key Save Failed"));

const valid = computed(() => isHexKey(key.value.trim()));
const onBoard = computed(() => valid.value && device.radioKey != null && key.value.trim().toLowerCase() === device.radioKey.toLowerCase());
const loading = computed(() => device.keyReadState === "loading");
const loadMessage = computed(() => ({
  loading: "Loading saved key…",
  empty: "No Key Saved",
  timeout: "Key Read Failed: No Reply",
  invalid: "Key Read Failed: Invalid Reply",
}[device.keyReadState] || ""));
watch(() => device.radioKey, (value) => { key.value = value ?? ""; }, { immediate: true });
watch(key, () => { lastResult.value = null; });
watch(() => device.identity?.devid, () => {
  key.value = device.radioKey ?? "";
  visible.value = false;
  lastResult.value = null;
});

function generate() {
  const buf = new Uint8Array(32);
  crypto.getRandomValues(buf);
  key.value = Array.from(buf, (b) => b.toString(16).padStart(2, "0")).join("");
  lastResult.value = null;
}

async function send() {
  if (!valid.value || onBoard.value || busy.value || loading.value || !device.connected || timing.armed) return;
  if (!window.confirm("Apply this radio key to the connected board?")) return;
  busy.value = true;
  failure.value = "";
  lastResult.value = null;
  try {
    const result = await device.provisionKey(key.value.trim());
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
  <section class="settings-section" aria-labelledby="radio-key-title">
    <div class="section-heading">
      <h4 id="radio-key-title">Radio Key</h4>
    </div>
    <div class="section-content">
      <div class="key-field">
        <input v-model.trim="key" class="form-input mono key-input" :type="visible ? 'text' : 'password'" :disabled="loading" maxlength="64" aria-label="Radio Key" spellcheck="false" autocomplete="off" placeholder="64 hex characters" />
        <button class="btn btn-ghost btn-sm visibility-toggle" type="button" :aria-pressed="visible" :aria-label="visible ? 'Hide Radio Key' : 'Show Radio Key'" @click="visible = !visible">{{ visible ? 'Hide' : 'Show' }}</button>
      </div>
      <p v-if="loadMessage" class="key-meta load-message">{{ loadMessage }}</p>
      <div v-if="key && !valid" class="key-meta">
        <span class="bad">Enter 64 Hex Characters</span>
      </div>

      <div class="section-actions">
        <button class="btn btn-primary btn-sm" type="button" :disabled="loading" @click="generate">Generate</button>
        <span v-if="lastResult" class="action-result" :class="lastResult === 'ok' ? 'ok' : 'bad'" role="status">
          {{ resultText }}
        </span>
        <div class="apply-group">
          <span v-if="!lastResult && device.connected && valid && !loading && !onBoard" class="key-status unsaved">Unsaved Changes</span>
          <button class="btn btn-success btn-sm send" type="button" :disabled="!valid || onBoard || !device.connected || busy || loading || timing.armed" @click="send">
            {{ busy ? "Applying…" : "Apply" }}
          </button>
        </div>
      </div>

    </div>
  </section>
</template>

<style scoped>
.key-field { display: flex; align-items: center; gap: 0.5rem; }
.key-input { min-width: 0; }
.visibility-toggle { flex-shrink: 0; min-width: 4rem; }
.load-message { color: var(--text-secondary); }
.key-status { font-size: 0.8rem; text-align: right; }
.unsaved { color: var(--accent-warning); }
.key-input {
  letter-spacing: 0.02em;
}
.key-meta {
  margin-top: 0.3rem;
  font-size: 0.8rem;
}
.send {
  flex-shrink: 0;
}
.ok {
  color: var(--accent-success);
}
.bad {
  color: var(--accent-danger);
}
</style>
