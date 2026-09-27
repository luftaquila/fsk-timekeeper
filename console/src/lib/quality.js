/* Pre-arm / while-armed quality gate — port of the FSK server bridge service.
 * Missing diagnostics are uncertainty, not evidence that a capture was lost. */
import { REQUIRED_ROLES, WIRELESS_STATUS_MAX_AGE_MS, WIRELESS_SYNC_MAX_AGE_MS, WIRELESS_MAX_SKEW_PPM } from "./constants";

export function telemetryAgeMs(telemetry, now = Date.now()) {
  const seen = telemetry?.last_seen_at;
  return Number.isFinite(seen) ? Math.max(0, now - seen) : Infinity;
}

// mappings: enabled rows whose role the mode requires [{ node_id, role }]
// telemetry: { [node_id]: normalized telemetry } (node "0" = master)
// masterFresh: the USB link is up and the master has spoken recently
export function wirelessQuality({ mode, mappings, telemetry, masterFresh, now = Date.now() }) {
  const reasons = [];
  if (!masterFresh) reasons.push({ node_id: "0", reason: "The master is not connected." });

  const master = telemetry?.["0"];
  if (!master || telemetryAgeMs(master, now) > WIRELESS_STATUS_MAX_AGE_MS) {
    reasons.push({ node_id: "0", reason: "No recent status report from the master." });
  } else {
    if (master.link_state !== "online") reasons.push({ node_id: "0", reason: "The master is not in a healthy state." });
    if (master.clock_source !== "xtal") reasons.push({ node_id: "0", reason: "The master crystal oscillator (HFXO) is not confirmed." });
    if (master.provisioned !== 1) reasons.push({ node_id: "0", reason: "The master has no radio key." });
  }

  const requiredRoles = REQUIRED_ROLES[mode] || [];
  for (const role of requiredRoles) {
    if (!mappings.some((row) => row.role === role)) reasons.push({ node_id: null, role, reason: `No ${role} sensor is mapped.` });
  }

  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    const t = telemetry?.[node];
    if (!t || telemetryAgeMs(t, now) > WIRELESS_STATUS_MAX_AGE_MS) {
      reasons.push({ node_id: node, role: mapping.role, reason: `No recent status report from sensor ${node}.` });
      continue;
    }
    if (t.link_state !== "online") reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} link is not healthy.` });
    if (t.provisioned !== 1) reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} has no radio key.` });
    if (t.clock_source !== "xtal") reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} crystal oscillator (HFXO) is not confirmed.` });
    if (t.sync_valid !== 1 || !Number.isFinite(t.sync_age_ms) || t.sync_age_ms > WIRELESS_SYNC_MAX_AGE_MS) {
      reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} clock sync is not valid.` });
    }
    if (t.skew_valid !== 1 || !Number.isFinite(t.skew_ppm) || Math.abs(t.skew_ppm) > WIRELESS_MAX_SKEW_PPM) {
      reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} clock skew is not valid.` });
    }
  }
  return { ok: reasons.length === 0, reasons, mappings };
}

export function missingRoles(mode, mappings) {
  const have = new Set(mappings.map((m) => m.role));
  return (REQUIRED_ROLES[mode] || []).filter((r) => !have.has(r));
}
