<script setup>
defineProps({
  value: { type: [String, Number], default: "—" },
  unit: { type: String, default: "" },
  parenthesized: { type: Boolean, default: false },
});
</script>

<template>
  <span class="metric-value" :class="{ parenthesized }">
    <span class="metric-number">{{ value ?? "—" }}</span>{{ " " }}<span v-if="unit" class="metric-unit">{{ unit }}</span>
  </span>
</template>

<style scoped>
.metric-value { display: inline-flex; align-items: baseline; gap: 0.3rem; white-space: nowrap; }
.parenthesized::before { content: "("; margin-right: -0.3rem; }
.parenthesized::after { content: ")"; margin-left: -0.3rem; }
.metric-number { font-family: var(--font-mono); font-size: 0.95rem; font-weight: 600; font-variant-numeric: tabular-nums; }
.metric-unit { font-family: var(--font-sans); font-size: 0.875rem; font-weight: 400; color: color-mix(in srgb, var(--text-primary) 65%, var(--text-secondary)); }
</style>
