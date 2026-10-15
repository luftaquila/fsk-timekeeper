<script setup>
import { ref, computed, watch, nextTick } from "vue";
import { useDeviceStore } from "../stores/device";

const device = useDeviceStore();
const open = ref(false);
const hideHeartbeat = ref(true);
const input = ref("");
const box = ref(null);

const lines = computed(() => (hideHeartbeat.value ? device.consoleLines.filter((l) => !(l.dir === "rx" && l.text.startsWith("H "))) : device.consoleLines));

watch(
  [open, () => lines.value.length, () => lines.value[lines.value.length - 1]],
  async () => {
    if (!open.value) return;
    await nextTick();
    if (box.value) box.value.scrollTop = box.value.scrollHeight;
  },
);

function fmt(t) {
  const d = new Date(t);
  return d.toLocaleTimeString([], { hour12: false }) + "." + String(d.getMilliseconds()).padStart(3, "0");
}

function send() {
  const s = input.value.trim();
  if (!s) return;
  device.transmitLine(s);
  input.value = "";
}
</script>

<template>
  <div class="card">
    <div class="card-header console-header" :class="{ 'is-open': open }" @click="open = !open">
      <h3><span class="heading-icon" aria-hidden="true">🖥️</span><span>Serial Console</span> <span class="muted">{{ device.stats.lines }} lines</span></h3>
      <span class="disclosure" :class="{ 'is-open': open }" aria-hidden="true">›</span>
    </div>
    <div v-if="open" class="card-body">
      <p class="muted console-stats">Events: {{ device.stats.events }} · ACKs: {{ device.stats.acks }} · Duplicates: {{ device.stats.duplicates }}</p>
      <label class="opt"><input type="checkbox" v-model="hideHeartbeat" /> Hide Heartbeat (H) Lines</label>
      <div ref="box" class="log mono">
        <div v-for="(l, i) in lines" :key="i" class="line" :class="l.dir">
          <span class="ts">{{ fmt(l.t) }}</span><span class="dir">{{ l.dir === "tx" ? "→" : "←" }}</span>{{ l.text }}
        </div>
        <div v-if="!lines.length" class="muted">No traffic yet.</div>
      </div>
      <form class="send" @submit.prevent="send">
        <input v-model="input" class="form-input mono" placeholder="?STATUS" :disabled="!device.connected" />
        <button class="btn btn-ghost btn-sm" type="submit" :disabled="!device.connected">Send</button>
      </form>
    </div>
  </div>
</template>

<style scoped>
.console-stats { margin-bottom: 0.75rem; }
.console-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  cursor: pointer;
  user-select: none;
}
.muted {
  color: var(--text-tertiary);
  font-weight: 400;
  font-size: 0.8rem;
}
.console-header:not(.is-open) { border-bottom: 0; }
.opt {
  display: flex;
  align-items: center;
  gap: 0.4rem;
  font-size: 0.8rem;
  color: var(--text-secondary);
  margin-bottom: 0.5rem;
}
.log {
  height: 16rem;
  overflow: auto;
  background: var(--bg-input);
  border: 1px solid var(--border-color);
  border-radius: 8px;
  padding: 0.5rem 0.75rem;
  font-size: 0.75rem;
  line-height: 1.5;
  white-space: pre;
}
.line.tx {
  color: var(--accent-primary);
}
.ts {
  color: var(--text-tertiary);
  margin-right: 0.5rem;
}
.dir {
  margin-right: 0.5rem;
}
.send {
  display: flex;
  gap: 0.5rem;
  margin-top: 0.5rem;
}
</style>
