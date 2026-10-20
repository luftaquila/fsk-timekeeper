import { defineStore } from "pinia";
import { reactive, computed, watch } from "vue";
import { MODES, ROLES, REQUIRED_ROLES, DEFAULT_DEBOUNCE_MS, DEBOUNCE_MIN_MS, DEBOUNCE_MAX_MS } from "../lib/constants";
import * as eventLog from "../lib/eventLog";
import { validateNodeId } from "../lib/protocol";

const KEY = "tk.settings.v2";

function defaults() {
  return {
    mapping: {}, // NODE8HEX -> { role, note }; unused excludes a sensor from timing
    removedSensors: [], // hide cached LOST reports until the sensor is heard again
    mode: "sprint",
    lapTarget: null,
    lastNote: "",
    debounceMs: DEFAULT_DEBOUNCE_MS,
  };
}

function clampDebounce(ms) {
  return Math.max(DEBOUNCE_MIN_MS, Math.min(DEBOUNCE_MAX_MS, Math.trunc(ms)));
}

function load() {
  const base = defaults();
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return base;
    const saved = JSON.parse(raw);
    if (saved && typeof saved === "object") {
      if (Array.isArray(saved.removedSensors)) {
        base.removedSensors = [...new Set(saved.removedSensors.filter((node) => typeof node === "string" && node !== "0" && validateNodeId(node)).map((node) => node.toUpperCase()))];
      }
      if (saved.mapping && typeof saved.mapping === "object") {
        for (const [node, m] of Object.entries(saved.mapping)) {
          if (!validateNodeId(node) || !m || !ROLES.includes(m.role)) continue;
          base.mapping[node.toUpperCase()] = { role: m.role, note: typeof m.note === "string" ? m.note : "" };
        }
      }
      if (MODES.includes(saved.mode)) base.mode = saved.mode;
      if (Number.isInteger(saved.lapTarget) && saved.lapTarget > 0) base.lapTarget = saved.lapTarget;
      if (typeof saved.lastNote === "string") base.lastNote = saved.lastNote;
      if (Number.isInteger(saved.debounceMs)) base.debounceMs = clampDebounce(saved.debounceMs);
    }
  } catch {
    /* corrupt storage - start clean */
  }
  return base;
}

export const useSettingsStore = defineStore("settings", () => {
  const state = reactive(load());

  watch(
    state,
    () => {
      try {
        localStorage.setItem(KEY, JSON.stringify(state));
      } catch (error) {
        eventLog.reportWriteFailure(error);
      }
    },
    { deep: true },
  );

  const mappingList = computed(() =>
    Object.entries(state.mapping)
      .map(([node_id, m]) => ({ node_id, ...m }))
      .sort((a, b) => a.node_id.localeCompare(b.node_id)),
  );

  // Only roles used by the mode participate; unused sensors stay in the mapping.
  function mappingsFor(mode) {
    const roles = REQUIRED_ROLES[mode] || [];
    return mappingList.value.filter((m) => roles.includes(m.role));
  }

  function mappingOf(node) {
    const id = String(node).toUpperCase();
    const m = state.mapping[id];
    return m ? { node_id: id, ...m } : null;
  }

  function setMapping(node, { role, note = "" }) {
    const id = String(node).toUpperCase();
    if (!validateNodeId(id) || id === "0") throw new Error("Invalid node id.");
    if (!ROLES.includes(role)) throw new Error("Invalid role.");
    restoreSensor(id);
    state.mapping[id] = { role, note: String(note ?? "") };
  }

  function removeMapping(node) {
    delete state.mapping[String(node).toUpperCase()];
  }

  function removeSensor(node) {
    const id = String(node).toUpperCase();
    if (!validateNodeId(id) || id === "0") return;
    removeMapping(id);
    if (!state.removedSensors.includes(id)) state.removedSensors.push(id);
  }

  function restoreSensor(node) {
    const index = state.removedSensors.indexOf(String(node).toUpperCase());
    if (index !== -1) state.removedSensors.splice(index, 1);
  }

  function setMode(mode) {
    if (!MODES.includes(mode)) throw new Error("Invalid mode.");
    state.mode = mode;
  }

  function setLapTarget(value) {
    const v = Number.parseInt(value, 10);
    state.lapTarget = Number.isFinite(v) && v > 0 ? v : null;
  }

  function rememberNote(note) {
    state.lastNote = String(note ?? "");
  }

  function setDebounceMs(ms) {
    const v = Number.parseInt(ms, 10);
    state.debounceMs = Number.isFinite(v) ? clampDebounce(v) : DEFAULT_DEBOUNCE_MS;
  }

  return {
    state,
    mappingList,
    mappingsFor,
    mappingOf,
    setMapping,
    removeMapping,
    removeSensor,
    restoreSensor,
    setMode,
    setLapTarget,
    rememberNote,
    setDebounceMs,
  };
});
