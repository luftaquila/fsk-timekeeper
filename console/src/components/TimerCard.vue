<script setup>
import { useTimingStore } from "../stores/timing";

defineProps({
  title: { type: String, required: true },
  note: { type: String, default: "" },
});

const timing = useTimingStore();

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
      </div>
    </div>
  </div>
</template>

<style scoped>
.run-note {
  margin-top: -0.25rem;
  font-size: 1.25rem;
  color: var(--text-secondary);
  word-break: break-word;
}
</style>
