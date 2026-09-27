<script setup>
import { computed } from "vue";
import { ROLE_LABEL } from "../lib/constants";

// source = timing.source; note = the run note input
const props = defineProps({
  source: { type: Object, required: true },
  note: { type: String, default: "" },
});

const missing = computed(() => props.source.missingRoles);
const quality = computed(() => props.source.quality);
const canStart = computed(() => props.source.connected && !props.source.armed && !props.source.starting);
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🎛️ Control</h3></div>
    <div class="card-body">
      <div class="btn-group">
        <button class="btn btn-success" :disabled="!canStart" @click="source.start(note)">{{ source.starting ? "Starting…" : "Start" }}</button>
        <button class="btn btn-danger" :disabled="!source.armed" @click="source.stop()">Stop</button>
      </div>
      <button class="btn btn-ghost btn-block mt-1" :disabled="!source.run || source.armed" @click="source.reset()">Clear</button>
      <p v-if="!source.connected" class="note bad">Master not connected.</p>
      <p v-else-if="missing.length" class="note bad">Unmapped role: {{ missing.map((r) => ROLE_LABEL[r] || r).join(", ") }} — see Settings.</p>
      <p v-else-if="!quality.ok" class="note warn" :title="quality.reasons.map((r) => r.reason).join('\n')">
        Not ready: {{ quality.reasons[0].reason }}<span v-if="quality.reasons.length > 1"> (+{{ quality.reasons.length - 1 }})</span>
      </p>
      <p v-else class="note ok">Ready.</p>
    </div>
  </div>
</template>

<style scoped>
.note {
  margin-top: 0.75rem;
  font-size: 0.8rem;
}
.note.ok {
  color: var(--accent-success);
}
.note.warn {
  color: var(--accent-warning);
}
.note.bad {
  color: var(--accent-danger);
}
</style>
