// Run <-> JSON for localStorage. A run stored by another run format or protocol version is
// closed as invalid on load (its evidence cannot be judged by this console); history keeps it.
import { DEFAULT_DEBOUNCE_MS } from "./constants";
import { RUN_SCHEMA, invalidateRun } from "./engine";
import { RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "./protocol";

export const PROTOCOL_CHANGE_REASON = "Closed by a protocol change.";

export function encodeRun(run) {
  return JSON.stringify(run, (_key, value) => (typeof value === "bigint" ? String(value) : value));
}

// -> { run, protocolChanged } | null
export function decodeRun(text, { now = Date.now() } = {}) {
  if (!text) return null;
  let run;
  try {
    run = JSON.parse(text);
  } catch {
    return null;
  }
  if (!run || typeof run !== "object" || !run.runId || !run.mode) return null;
  if (run.schema !== RUN_SCHEMA || run.radioProto !== RADIO_PROTO_VERSION || run.usbProto !== USB_PROTO_VERSION) {
    // A finished run keeps its result (read-only); an open one has no evidence this console can judge.
    if (run.closed) return { run: { ...run, armed: false }, protocolChanged: false };
    return { run: invalidateRun(run, [{ node_id: null, reason: PROTOCOL_CHANGE_REASON }], { kind: "protocol", now }), protocolChanged: true };
  }
  if (!Number.isInteger(run.debounceMs) || run.debounceMs < 0) run.debounceMs = DEFAULT_DEBOUNCE_MS;
  if (!Array.isArray(run.crossingTicks)) run.crossingTicks = [];
  return { run, protocolChanged: false };
}
