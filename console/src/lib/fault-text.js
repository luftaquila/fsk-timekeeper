// Human text for the interval of a fault reason (ticks relative to the run's START boundary).
import { nominalNs } from "./event-timing";
import { formatDuration } from "./format";

function offset(tick, boundary) {
  return `+${formatDuration(nominalNs(BigInt(tick) - BigInt(boundary)))}`;
}

export function faultWindow(reason, boundaryTick) {
  if (!reason || reason.lo == null || boundaryTick == null) return "";
  const from = BigInt(reason.lo) < BigInt(boundaryTick) ? "START" : offset(reason.lo, boundaryTick);
  if (reason.hi === "inf") return `from ${from} on`;
  if (reason.hi == null) return `after ${from} (end not known yet)`;
  return `between ${from} and ${offset(reason.hi, boundaryTick)}`;
}
