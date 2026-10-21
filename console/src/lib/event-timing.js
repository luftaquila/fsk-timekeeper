// Master ticks: 16 MHz TIMER1 counts as decimal strings / BigInt (pure); seconds: calibration.js.

export const MASTER_TICKS_PER_MS = 16000n;
export const MASTER_TICKS_PER_S = 16_000_000n;
const MASTER_TICK_MAX = (1n << 64n) - 1n;

// Decimal string / safe integer / bigint -> bigint in [0, 2^64); throws otherwise.
export function masterTick(value) {
  let tick;
  if (typeof value === "bigint") tick = value;
  else if (typeof value === "number" && Number.isSafeInteger(value)) tick = BigInt(value);
  else if (typeof value === "string" && /^\d{1,20}$/.test(value)) tick = BigInt(value);
  if (tick != null && tick >= 0n && tick <= MASTER_TICK_MAX) return tick;
  throw new TypeError("invalid master tick");
}

// Tick span as ns at the nominal rate (non-result displays, e.g. fault windows).
export function nominalNs(ticks) {
  const t = BigInt(ticks);
  return t < 0n ? -nominalNs(-t) : (t * 125n + 1n) / 2n; // ×62.5, half up
}
