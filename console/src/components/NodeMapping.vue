<script setup>
import { useSettingsStore } from "../stores/settings";
import { ROLES, ROLE_LABEL } from "../lib/constants";

// Separate from MappingCard so its clock and telemetry re-renders never patch the
// <select>: Firefox rebuilds an open dropdown on any mutation and drops the pick.
const props = defineProps({ nodeId: { type: String, required: true } });
const settings = useSettingsStore();

function updateMapping(changes) {
  const current = settings.mappingOf(props.nodeId) ?? { role: "unused", note: "" };
  settings.setMapping(props.nodeId, { ...current, ...changes });
}
</script>

<template>
  <div class="mapping-controls">
    <label class="sensor-role">
      <span>Role</span>
      <select :value="settings.mappingOf(nodeId)?.role ?? 'unused'" class="form-select" aria-label="Role" @change="updateMapping({ role: $event.target.value })">
        <option v-for="r in ROLES" :key="r" :value="r">{{ ROLE_LABEL[r] }}</option>
      </select>
    </label>
    <label class="sensor-note">
      <input :value="settings.mappingOf(nodeId)?.note ?? ''" class="form-input" aria-label="Note" placeholder="Note" @input="updateMapping({ note: $event.target.value })" />
    </label>
  </div>
</template>

<style scoped>
.mapping-controls {
  display: flex;
  flex: 1 1 18rem;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 0.75rem 15px;
  align-items: center;
  min-width: 0;
}
.mapping-controls label { display: flex; align-items: center; gap: 0.5rem; min-width: 0; color: var(--text-secondary); font-size: 1rem; }
.mapping-controls label > span { flex-shrink: 0; }
.mapping-controls .form-input, .mapping-controls .form-select { height: 2.5rem; padding: 0.375rem 0.625rem; font-size: 1rem; }
.mapping-controls .form-select { width: 6.5rem; }
.mapping-controls .sensor-role { color: var(--text-primary); font-size: 1rem; font-weight: 600; }
.sensor-role .form-select { width: 7.25rem; font-weight: 600; color: var(--text-primary); }
.sensor-note { flex: 0 1 8rem; max-width: 100%; }
.sensor-note .form-input { min-width: 0; width: 100%; }
@container sensor-layout (min-width: 888px) {
  .mapping-controls { grid-column: 3 / -1; flex-wrap: nowrap; justify-content: flex-start; }
  .mapping-controls > label:first-child { flex-shrink: 0; }
  .sensor-note { flex: 1 1 0; max-width: none; }
}
</style>
