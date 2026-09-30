/* START gate. Link states are the firmware's (D line); the console only checks that the
 * report is recent. Missing diagnostics are uncertainty, not evidence of a lost capture. */
import { REQUIRED_ROLES, WIRELESS_STATUS_MAX_AGE_MS, WIRELESS_MAX_SKEW_PPM, QUEUE_HEALTH_RATIO, HEAD_STUCK_MS } from "./constants";
import { MASTER_QUEUE_CAPACITY } from "./protocol";

const STILL_ARRIVING_MS = 1000; // the master re-sends its head every 100 ms

// Time since the D line arrived.
export function telemetryAgeMs(telemetry, now = Date.now()) {
  const at = telemetry?.received_at;
  return Number.isFinite(at) ? Math.max(0, now - at) : Infinity;
}

export function telemetryFresh(telemetry, now = Date.now()) {
  return telemetryAgeMs(telemetry, now) <= WIRELESS_STATUS_MAX_AGE_MS;
}

// master: D 0 telemetry. head: { hseq, firstSeenAt, lastSeenAt } of the latest E line.
export function pipelineHealth({ master, head, connected, now = Date.now() }) {
  if (master && Number.isInteger(master.queue_depth) && master.queue_depth > MASTER_QUEUE_CAPACITY * QUEUE_HEALTH_RATIO) {
    return { ok: false, reason: `The master's host queue is nearly full (${master.queue_depth}/${MASTER_QUEUE_CAPACITY}).` };
  }
  if (connected && head && now - head.firstSeenAt > HEAD_STUCK_MS && now - head.lastSeenAt <= STILL_ARRIVING_MS) {
    return { ok: false, reason: `The master keeps re-sending event #${head.hseq}; the console is not acknowledging it.` };
  }
  return { ok: true, reason: null };
}

// mappings: enabled rows whose role the mode requires [{ node_id, role }]
// telemetry: { [node_id]: normalized telemetry } (node "0" = master)
// masterFresh: the USB link is up and the master has spoken recently
export function wirelessQuality({ mode, mappings, telemetry, masterFresh, contract = { ok: true }, pipeline = { ok: true }, now = Date.now() }) {
  const reasons = [];
  if (!masterFresh) reasons.push({ node_id: "0", reason: "The master is not connected." });
  else if (!contract.ok) reasons.push({ node_id: "0", reason: contract.reason });
  if (!pipeline.ok) reasons.push({ node_id: "0", reason: pipeline.reason });

  const master = telemetry?.["0"];
  if (!master || !telemetryFresh(master, now)) {
    reasons.push({ node_id: "0", reason: "No recent status report from the master." });
  } else {
    if (master.link_state !== "online") reasons.push({ node_id: "0", reason: "The master is not in a healthy state." });
    if (master.clock_source !== "xtal") reasons.push({ node_id: "0", reason: "The master crystal oscillator (HFXO) is not confirmed." });
    if (master.provisioned !== 1) reasons.push({ node_id: "0", reason: "The master has no radio key." });
  }

  for (const role of REQUIRED_ROLES[mode] || []) {
    if (!mappings.some((row) => row.role === role)) reasons.push({ node_id: null, role, reason: `No ${role} sensor is mapped.` });
  }

  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    const t = telemetry?.[node];
    if (!t || !telemetryFresh(t, now)) {
      reasons.push({ node_id: node, role: mapping.role, reason: `No recent status report from sensor ${node}.` });
      continue;
    }
    if (t.link_state !== "online") reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} link is not healthy.` });
    if (t.provisioned !== 1) reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} has no radio key.` });
    if (t.clock_source !== "xtal") reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} crystal oscillator (HFXO) is not confirmed.` });
    if (t.sync_valid !== 1) reasons.push({ node_id: node, role: mapping.role, reason: `Sensor ${node} clock sync is not valid.` });
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
