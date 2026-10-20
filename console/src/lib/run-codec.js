// Run <-> JSON. A run of another format or protocol version is not restored: its evidence
// cannot be judged by this console.
import { RUN_SCHEMA } from "./engine";
import { RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "./protocol";

export function encodeRun(run) {
  return JSON.stringify(run, (_key, value) => (typeof value === "bigint" ? String(value) : value));
}

// -> run | null
export function decodeRun(text) {
  if (!text) return null;
  let run;
  try {
    run = JSON.parse(text);
  } catch {
    return null;
  }
  if (!run || typeof run !== "object" || !run.runId || !run.mode) return null;
  if (run.schema !== RUN_SCHEMA || run.radioProto !== RADIO_PROTO_VERSION || run.usbProto !== USB_PROTO_VERSION) return null;
  return run;
}
