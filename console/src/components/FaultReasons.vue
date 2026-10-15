<script setup>
import { ROLE_LABEL } from "../lib/constants";
import { faultWindow, formatFaultReason } from "../lib/fault-text";
defineProps({ reasons: { type: Array, default: () => [] }, boundaryTick: { type: [String, Number, BigInt], default: null } });
</script>

<template>
  <ul class="fault-reasons">
    <li v-for="(reason, i) in reasons" :key="i">
      <span v-if="reason.role" class="fault-role">{{ ROLE_LABEL[reason.role] || reason.role }}</span>
      <span>{{ formatFaultReason(reason.reason) }}</span>
      <span v-if="faultWindow(reason, boundaryTick)" class="fault-window">{{ faultWindow(reason, boundaryTick) }}</span>
    </li>
  </ul>
</template>

<style scoped>
.fault-reasons { margin: 0.35rem 0 0; padding-left: 1.2rem; font-size: 0.85rem; overflow-wrap: anywhere; }
li + li { margin-top: 0.6rem; }
.fault-role { display: block; font-weight: 600; color: var(--text-secondary); }
.fault-window { display: block; margin-top: 0.15rem; color: var(--text-secondary); font-size: 0.8rem; }
</style>
