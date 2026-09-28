/* Master-tick arithmetic (pure). Official results subtract raw 64-bit ticks and round to ms once.
 *
 * `ppb` is the master HFXO error measured against GPS PPS (parts per billion, positive = the
 * 16 MHz timebase runs fast). 0 = nominal 16 000 ticks/ms. With ppb the real rate is
 * 16e6·(1e9+ppb)/1e9 ticks/s, so ms = ticks·62500/(1e9+ppb), rounded half up in exact integers.
 */

export const MASTER_TICKS_PER_MS = 16000n;
export const MAX_PPB = 1_000_000;
const MASTER_TICK_MAX = (1n << 64n) - 1n;

function masterTick(value) {
  let tick;
  if (typeof value === "bigint") tick = value;
  else if (typeof value === "number" && Number.isSafeInteger(value)) tick = BigInt(value);
  else if (typeof value === "string" && /^\d{1,20}$/.test(value)) tick = BigInt(value);
  if (tick != null && tick >= 0n && tick <= MASTER_TICK_MAX) return tick;
  throw new TypeError("invalid master tick");
}

function checkPpb(ppb) {
  if (!Number.isInteger(ppb) || Math.abs(ppb) > MAX_PPB) throw new TypeError("invalid ppb");
  return BigInt(ppb);
}

// ticks (bigint, may be negative) -> ms, rounded half away from zero, exact rational arithmetic.
function roundTickDuration(ticks, ppb = 0) {
  const d = 1_000_000_000n + checkPpb(ppb);
  if (ticks < 0n) return -Number((-ticks * 125000n + d) / (2n * d));
  return Number((ticks * 125000n + d) / (2n * d));
}

export function masterTickDelta(end, start) {
  return masterTick(end) - masterTick(start);
}

export function masterTickDeltaMs(end, start, ppb = 0) {
  return roundTickDuration(masterTickDelta(end, start), ppb);
}

export function masterTickDurationsMs(durations, ppb = 0) {
  return roundTickDuration((durations || []).reduce((sum, value) => sum + masterTick(value), 0n), ppb);
}

// A signed tick count (bigint / safe integer / decimal string) -> ms.
export function tickDurationMs(ticks, ppb = 0) {
  let t;
  if (typeof ticks === "bigint") t = ticks;
  else if (typeof ticks === "number" && Number.isSafeInteger(ticks)) t = BigInt(ticks);
  else if (typeof ticks === "string" && /^-?\d{1,20}$/.test(ticks)) t = BigInt(ticks);
  else throw new TypeError("invalid tick count");
  return roundTickDuration(t, ppb);
}

// Debounce windows compare raw ticks at the nominal rate; a ppm-level scale is irrelevant here.
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
