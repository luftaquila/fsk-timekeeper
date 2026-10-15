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

// Older saved runs retain their original evidence; normalize known labels for display.
const LEGACY_REASONS = {
  "lost captures": "Capture Loss",
  "lost a capture at an unknown time": "Capture Loss (Time Unknown)",
  "captured an edge without a valid clock": "Invalid Capture Clock",
  "reported capture times out of order": "Capture Times Out of Order",
  "sent an event the console quarantined": "Invalid Event Quarantined",
  "rebooted during the run": "Reboot During Run"
};

export function formatFaultReason(value) {
  const text = String(value ?? "Unknown Cause");
  const match = /^Sensor (\S+) (.+)\.$/.exec(text);
  return match && LEGACY_REASONS[match[2]] ? `Sensor ${match[1]}: ${LEGACY_REASONS[match[2]]}` : text;
}
