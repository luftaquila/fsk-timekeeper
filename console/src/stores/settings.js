import { defineStore } from "pinia";
import { reactive, computed, watch } from "vue";
import { MODES, ROLES, REQUIRED_ROLES, DEFAULT_DEBOUNCE_MS, DEBOUNCE_MIN_MS, DEBOUNCE_MAX_MS } from "../lib/constants";
import { validateNodeId } from "../lib/protocol";

const KEY = "tk.settings.v2";
const OLD_KEYS = ["tk.settings.v1", "tk.runs.v1", "tk.fleetKey.v1"];

function defaults() {
  return {
    mapping: {}, // NODE8HEX -> { role, note, enabled }
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
    for (const k of OLD_KEYS) localStorage.removeItem(k);
    const raw = localStorage.getItem(KEY);
    if (!raw) return base;
    const saved = JSON.parse(raw);
    if (saved && typeof saved === "object") {
      if (saved.mapping && typeof saved.mapping === "object") {
        for (const [node, m] of Object.entries(saved.mapping)) {
          if (!validateNodeId(node) || !m || !ROLES.includes(m.role)) continue;
          base.mapping[node.toUpperCase()] = { role: m.role, note: typeof m.note === "string" ? m.note : "", enabled: m.enabled !== false };
        }
      }
      if (MODES.includes(saved.mode)) base.mode = saved.mode;
      if (Number.isInteger(saved.lapTarget) && saved.lapTarget > 0) base.lapTarget = saved.lapTarget;
      if (typeof saved.lastNote === "string") base.lastNote = saved.lastNote;
      if (Number.isInteger(saved.debounceMs)) base.debounceMs = clampDebounce(saved.debounceMs);
    }
  } catch {
    /* corrupt storage — start clean */
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
      } catch {
        /* quota / private mode */
      }
    },
    { deep: true },
  );

  const mappingList = computed(() =>
    Object.entries(state.mapping)
      .map(([node_id, m]) => ({ node_id, ...m }))
      .sort((a, b) => a.node_id.localeCompare(b.node_id)),
  );

  // Enabled sensors whose role the mode uses; other roles are ignored by that mode.
  function mappingsFor(mode) {
    const roles = REQUIRED_ROLES[mode] || [];
    return mappingList.value.filter((m) => m.enabled && roles.includes(m.role));
  }

  function mappingOf(node) {
    const id = String(node).toUpperCase();
    const m = state.mapping[id];
    return m ? { node_id: id, ...m } : null;
  }

  function setMapping(node, { role, note = "", enabled = true }) {
    const id = String(node).toUpperCase();
    if (!validateNodeId(id) || id === "0") throw new Error("Invalid node id.");
    if (!ROLES.includes(role)) throw new Error("Invalid role.");
    state.mapping[id] = { role, note: String(note ?? ""), enabled: !!enabled };
  }

  function removeMapping(node) {
    delete state.mapping[String(node).toUpperCase()];
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
    setMode,
    setLapTarget,
    rememberNote,
    setDebounceMs,
  };
});
