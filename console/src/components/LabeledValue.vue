<script setup>
/* One labelled value in a row of them (a header, a list row, a dialog): the label muted, a figure
 * as MetricValue, or wrapping text with `text`. */
import MetricValue from "./MetricValue.vue";

defineProps({
  label: { type: String, required: true },
  value: { type: [String, Number], default: "-" },
  unit: { type: String, default: "" },
  text: { type: Boolean, default: false },
});
</script>

<template>
  <span class="labeled-value" :class="{ text }">
    <span class="label">{{ label }}</span>{{ " " }}<span v-if="text" class="text-value">{{ value }}</span>
    <MetricValue v-else :value="value" :unit="unit" />
  </span>
</template>

<style scoped>
.labeled-value { display: inline-flex; align-items: baseline; gap: 0.4rem; white-space: nowrap; }
.labeled-value.text { white-space: normal; min-width: 0; }
.label { color: var(--text-secondary); font-size: 1rem; }
.text-value { color: var(--text-primary); font-size: 1rem; overflow-wrap: anywhere; }
</style>
