<script setup>
import { computed } from "vue";
import { useTimingStore } from "../stores/timing";

defineProps({
  title: { type: String, required: true },
  note: { type: String, default: "" },
});

const timing = useTimingStore();

const badge = computed(() => {
  const run = timing.run;
  if (!run) return null;
  if (run.verification === "verified") return { cls: "badge-success", text: run.closed ? "Verified" : "Verified so far" };
  if (run.verification === "invalid") return { cls: "badge-danger", text: "Invalid" };
  if (run.verification === "dnf") return { cls: "badge-default", text: run.dnfReason === "DNS" ? "DNS — no start" : "DNF — no finish" };
  return { cls: "badge-warning", text: run.armed ? "Measuring — awaiting confirmation" : "Pending" };
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
          <span class="traffic-light" :class="timing.lightColor"></span>
          <span class="clock">{{ timing.live.clockDisplay }}</span>
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
