/* Master-tick arithmetic (pure). Official results subtract raw 64-bit ticks and round to ms once. */

export const MASTER_TICKS_PER_MS = 16000n;
const MASTER_TICK_MAX = (1n << 64n) - 1n;

function masterTick(value) {
  let tick;
  if (typeof value === "bigint") tick = value;
  else if (typeof value === "number" && Number.isSafeInteger(value)) tick = BigInt(value);
  else if (typeof value === "string" && /^\d{1,20}$/.test(value)) tick = BigInt(value);
  if (tick != null && tick >= 0n && tick <= MASTER_TICK_MAX) return tick;
  throw new TypeError("invalid master tick");
}

function roundTickDuration(ticks) {
  if (ticks < 0n) return -Number((-ticks + MASTER_TICKS_PER_MS / 2n) / MASTER_TICKS_PER_MS);
  return Number((ticks + MASTER_TICKS_PER_MS / 2n) / MASTER_TICKS_PER_MS);
}

export function masterTickDelta(end, start) {
  return masterTick(end) - masterTick(start);
}

export function masterTickDeltaMs(end, start) {
  return roundTickDuration(masterTickDelta(end, start));
}

export function masterTickDurationsMs(durations) {
  return roundTickDuration((durations || []).reduce((sum, value) => sum + masterTick(value), 0n));
}

export function masterTickDistanceBelowMs(a, b, windowMs) {
  if (!Number.isInteger(windowMs) || windowMs < 0) return false;
  const delta = masterTick(a) - masterTick(b);
  const distance = delta < 0n ? -delta : delta;
  return distance < BigInt(windowMs) * MASTER_TICKS_PER_MS;
}

// ms -> "MM:SS.mmm"
export function formatLapMs(ms) {
  if (!Number.isFinite(ms) || ms < 0) ms = 0;
  ms = Math.round(ms);
  const m = String(Math.floor(ms / 60000)).padStart(2, "0");
  const s = String(Math.floor((ms % 60000) / 1000)).padStart(2, "0");
  const f = String(ms % 1000).padStart(3, "0");
  return `${m}:${s}.${f}`;
}
