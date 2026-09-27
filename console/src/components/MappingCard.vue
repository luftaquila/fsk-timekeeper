<script setup>
import { reactive, computed, watch } from "vue";
import { useDeviceStore } from "../stores/device";
import { useSettingsStore } from "../stores/settings";
import { useNotification } from "../composables/useNotification";
import { ROLES, ROLE_LABEL } from "../lib/constants";

const notyf = useNotification();
const device = useDeviceStore();
const settings = useSettingsStore();

// Discovered nodes = telemetry ∪ saved mapping, master excluded.
const nodes = computed(() => {
  const s = new Set([...Object.keys(device.telemetry), ...settings.mappingList.map((m) => m.node_id)]);
  s.delete("0");
  return [...s].sort();
});

const draft = reactive({});
function seed() {
  for (const node of nodes.value) {
    if (draft[node]?._dirty) continue;
    const m = settings.mappingOf(node);
    draft[node] = m ? { role: m.role, note: m.note || "", enabled: m.enabled !== false, _dirty: false } : { role: "start", note: "", enabled: true, _dirty: false };
  }
}
watch([nodes, () => settings.mappingList], seed, { immediate: true, deep: true });

function markDirty(node) {
  if (draft[node]) draft[node]._dirty = true;
}

function isSaved(node) {
  return !!settings.mappingOf(node) && !draft[node]?._dirty;
}

function save(node) {
  const d = draft[node];
  try {
    settings.setMapping(node, { role: d.role, note: d.note, enabled: d.enabled });
    d._dirty = false;
    notyf.success(`Sensor ${node} mapped`);
  } catch (e) {
    notyf.error(e.message);
  }
}

function remove(node) {
  settings.removeMapping(node);
  delete draft[node];
  seed();
  notyf.success(`Sensor ${node} mapping removed`);
}

function online(node) {
  return device.telemetry[node]?.link_state === "online";
}
</script>

<template>
  <div class="card">
    <div class="card-header"><h3>🔗 Sensor mapping</h3></div>
    <div class="card-body">
      <div v-if="!nodes.length" class="empty-state">No sensors yet — connect the master and wait for sync.</div>
      <div v-else class="table-scroll">
        <table class="assign-table">
          <thead>
            <tr>
              <th>Node</th>
              <th>Role</th>
              <th>Note</th>
              <th>On</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="node in nodes" :key="node">
              <td class="mono node"><span class="ndot" :class="{ on: online(node) }"></span>{{ node }}</td>
              <td>
                <select v-model="draft[node].role" class="form-select" @change="markDirty(node)">
                  <option v-for="r in ROLES" :key="r" :value="r">{{ ROLE_LABEL[r] }}</option>
                </select>
              </td>
              <td><input v-model="draft[node].note" class="form-input" placeholder="optional" @input="markDirty(node)" /></td>
              <td class="center"><input type="checkbox" v-model="draft[node].enabled" @change="markDirty(node)" /></td>
              <td class="actions">
                <button class="btn btn-sm save-btn" :class="isSaved(node) ? 'btn-ghost is-saved' : 'btn-success'" :disabled="isSaved(node)" @click="save(node)">
                  {{ isSaved(node) ? "✓ Saved" : "Save" }}
                </button>
                <button class="btn btn-ghost btn-sm" :disabled="!settings.mappingOf(node)" @click="remove(node)">Remove</button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
.assign-table {
  width: 100%;
  min-width: 30rem;
  border-collapse: collapse;
}
.assign-table th,
.assign-table td {
  padding: 0.5rem 0.6rem;
  text-align: left;
  border-bottom: 1px solid var(--border-color);
  vertical-align: middle;
}
.assign-table th {
  color: var(--text-tertiary);
  font-weight: 600;
  font-size: 0.8rem;
}
.assign-table tbody tr:last-child td {
  border-bottom: none;
}
.assign-table .form-input,
.assign-table .form-select {
  padding: 0.45rem 0.6rem;
}
.node {
  font-weight: 700;
  white-space: nowrap;
}
.ndot {
  display: inline-block;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--text-tertiary);
  margin-right: 0.5rem;
}
.ndot.on {
  background: var(--accent-success);
}
.center {
  text-align: center;
}
.actions {
  display: flex;
  gap: 0.4rem;
  justify-content: flex-end;
}
.save-btn {
  min-width: 4.5rem;
}
.save-btn.is-saved {
  opacity: 0.6;
  cursor: default;
}
</style>
