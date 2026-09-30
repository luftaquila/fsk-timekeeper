<script setup>
import { computed, ref } from "vue";
import { useTimingStore } from "../stores/timing";
import { MODE_LABEL, ROLE_LABEL } from "../lib/constants";
import { faultWindow } from "../lib/fault-text";

const timing = useTimingStore();
const dismissed = ref(null);

const fault = computed(() => (timing.fault?.fault_id && timing.fault.fault_id !== dismissed.value ? timing.fault : null));

function title(f) {
  const label = MODE_LABEL[f.mode] || f.mode;
  if (f.kind === "protocol") return `${label}: run closed by a protocol change`;
  return f.kind === "measurement" ? `${label}: run invalidated by a measurement fault` : `${label}: run invalidated`;
}

function reasonText(reason) {
  const role = reason?.role ? `${ROLE_LABEL[reason.role] || reason.role}: ` : "";
  const span = faultWindow(reason, timing.run?.boundaryTick);
  return `${role}${reason?.reason || "Unknown cause."}${span ? ` (${span})` : ""}`;
}

function occurredAt(value) {
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? "" : date.toLocaleString();
}
</script>

<template>
  <section v-if="fault" class="quality-alerts" aria-live="assertive">
    <article class="quality-alert" role="alert">
      <div class="quality-alert-icon" aria-hidden="true">⚠️</div>
      <div class="quality-alert-content">
        <strong>{{ title(fault) }}</strong>
        <time v-if="occurredAt(fault.occurred_at)" :datetime="fault.occurred_at">{{ occurredAt(fault.occurred_at) }}</time>
        <ul>
          <li v-for="(reason, index) in fault.reasons || []" :key="index">{{ reasonText(reason) }}</li>
        </ul>
      </div>
      <button class="quality-alert-dismiss" type="button" aria-label="Dismiss" @click="dismissed = fault.fault_id">Dismiss</button>
    </article>
  </section>
</template>

<style scoped>
.quality-alerts {
  position: sticky;
  top: 0;
  z-index: 100;
  display: grid;
  gap: 0.5rem;
  padding: 0.75rem max(1rem, calc((100vw - 1400px) / 2));
  pointer-events: none;
}
.quality-alert {
  display: flex;
  align-items: flex-start;
  gap: 0.75rem;
  padding: 0.8rem 1rem;
  border: 1px solid rgba(239, 68, 68, 0.65);
  border-radius: 10px;
  background: color-mix(in srgb, var(--bg-primary) 90%, #ef4444 10%);
  box-shadow: 0 4px 16px rgba(127, 29, 29, 0.2);
  color: var(--text-primary);
  pointer-events: auto;
}
.quality-alert-icon {
  line-height: 1.4;
}
.quality-alert-content {
  flex: 1;
  min-width: 0;
}
.quality-alert-content strong {
  color: var(--accent-danger);
}
.quality-alert-content time {
  display: block;
  margin-top: 0.15rem;
  color: var(--text-tertiary);
  font-size: 0.8rem;
}
.quality-alert-content ul {
  margin: 0.35rem 0 0;
  padding-left: 1.25rem;
}
.quality-alert-content li {
  margin-top: 0.15rem;
}
.quality-alert-dismiss {
  flex: 0 0 auto;
  border: 1px solid rgba(239, 68, 68, 0.45);
  border-radius: 7px;
  padding: 0.35rem 0.7rem;
  background: transparent;
  color: inherit;
  cursor: pointer;
}
.quality-alert-dismiss:hover {
  background: rgba(239, 68, 68, 0.12);
}
</style>
