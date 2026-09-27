<script setup>
import { computed } from "vue";

const props = defineProps({
  source: { type: Object, required: true },
  title: { type: String, required: true },
  note: { type: String, default: "" },
});

const badge = computed(() => {
  const v = props.source.verification;
  if (!props.source.run) return null;
  if (v === "verified") return { cls: "badge-success", text: props.source.run.closed ? "Verified" : "Verified so far" };
  if (v === "invalid") return { cls: "badge-danger", text: "Invalid" };
  return { cls: "badge-warning", text: props.source.armed ? "Measuring — awaiting confirmation" : "Pending" };
});
</script>

<template>
  <div class="card">
    <div class="card-body">
      <div class="monitor-header">
        <h2 class="event-title">{{ title }}</h2>
        <div v-if="note" class="run-note">{{ note }}</div>
      </div>
      <div class="timer-section">
        <div class="timer-display">
          <span class="traffic-light" :class="source.lightColor"></span>
          <span class="clock">{{ source.clockDisplay }}</span>
        </div>
        <span v-if="badge" class="badge" :class="badge.cls">{{ badge.text }}</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.run-note {
  margin-top: -0.25rem;
  font-size: 1.1rem;
  color: var(--text-secondary);
  word-break: break-word;
}
</style>
