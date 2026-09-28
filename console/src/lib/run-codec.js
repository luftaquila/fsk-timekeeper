// Run <-> JSON (BigInt lapTicks as decimal strings), for localStorage persistence.
import { DEFAULT_DEBOUNCE_MS } from "./constants";

export function encodeRun(run) {
  return JSON.stringify(run, (_key, value) => (typeof value === "bigint" ? String(value) : value));
}

export function decodeRun(text) {
  if (!text) return null;
  try {
    const run = JSON.parse(text);
    if (!run || typeof run !== "object" || !run.runId || !run.mode) return null;
    run.lapTicks = (run.lapTicks || []).map((t) => BigInt(t));
    // Runs saved before the window was frozen into the run evaluate with the default.
    if (!Number.isInteger(run.debounceMs) || run.debounceMs < 0) run.debounceMs = DEFAULT_DEBOUNCE_MS;
    return run;
  } catch {
    return null;
  }
}
