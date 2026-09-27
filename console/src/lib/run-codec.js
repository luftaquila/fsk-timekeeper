// Run <-> JSON (BigInt lapTicks as decimal strings), for localStorage persistence.

export function encodeRun(run) {
  return JSON.stringify(run, (_key, value) => (typeof value === "bigint" ? String(value) : value));
}

export function decodeRun(text) {
  if (!text) return null;
  try {
    const run = JSON.parse(text);
    if (!run || typeof run !== "object" || !run.runId || !run.mode) return null;
    run.lapTicks = (run.lapTicks || []).map((t) => BigInt(t));
    return run;
  } catch {
    return null;
  }
}
