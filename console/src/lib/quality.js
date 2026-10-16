/* Shared current timing status and START gate. Missing diagnostics are uncertainty,
 * not evidence of a hardware fault or a lost capture. */
import { REQUIRED_ROLES, WIRELESS_STATUS_MAX_AGE_MS, WIRELESS_MAX_SKEW_PPM, WIRELESS_SYNC_TTL_MS, QUEUE_HEALTH_RATIO, HEAD_STUCK_MS } from "./constants";
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

// A valid diagnostic can report a fault. diag_valid only identifies its source.
export function timingHealth(report, { nodeId = report?.node_id, connected = true, admission, now = Date.now() } = {}) {
  const master = nodeId === "0";
  const who = master ? "Master" : `Sensor ${nodeId}`;
  const result = (label, state, reasons = [], syncAgeMs = null) => ({ ok: state === "good", label, state, reasons, syncAgeMs });
  if (!connected) return result("UNKNOWN", "warn", ["Master not connected"]);
  const fresh = telemetryFresh(report, now);
  // A pending replacement must not invalidate a still-usable active connection.
  if (!master && admission?.state === "WAIT" && now - admission.at <= WIRELESS_STATUS_MAX_AGE_MS &&
      !(fresh && report.link_state === "online" && report.diag_valid === 1)) {
    return result("CONNECTING", "warn", [`${who} is connecting`]);
  }
  if (!fresh) return result("UNKNOWN", "warn", [`No recent status report from ${master ? "master" : `sensor ${nodeId}`}`]);
  if (!master && report.link_state !== "online") {
    return result("UNKNOWN", "warn", [`${who} link: ${report.link_state === "degraded" ? "STALE" : report.link_state === "lost" ? "LOST" : "UNKNOWN"}`]);
  }
  if (report.diag_valid !== 1) return result(master ? "UNKNOWN" : "CONNECTING", "warn", [`${who} has no current diagnostics`]);
  const issues = [];
  if (report.provisioned !== 1) issues.push(["NO KEY SET", `${who} has no radio key`]);
  if (report.clock_source !== "xtal") issues.push([report.clock_source === "rc" ? "XTAL FAIL" : "UNKNOWN", `${who} HFXO not confirmed`]);
  let syncAgeMs = null;
  if (master) {
    if (report.link_state !== "online") issues.push(["UNKNOWN", `Master link: ${report.link_state === "degraded" ? "STALE" : "LOST"}`]);
  } else {
    // Firmware clears skew_valid outside the limit but still reports the measured drift.
    if (Number.isFinite(report.skew_ppm) && Math.abs(report.skew_ppm) > WIRELESS_MAX_SKEW_PPM) {
      issues.push(["CLOCK DRIFT", `${who} clock drift out of range (${report.skew_ppm} ppm)`]);
    }
    syncAgeMs = Number.isFinite(report.sync_age_ms) && report.sync_age_ms >= 0 ? report.sync_age_ms + telemetryAgeMs(report, now) : null;
    if (report.sync_valid !== 1 || syncAgeMs == null || syncAgeMs > WIRELESS_SYNC_TTL_MS) {
      issues.push(["SYNC INVALID", `${who} clock sync is not valid`]);
    }
    if (report.skew_valid !== 1 || !Number.isFinite(report.skew_ppm)) issues.push(["SYNC INVALID", `${who} clock skew is not valid`]);
  }
  if (issues.length) return result(issues[0][0], "bad", issues.map(([, reason]) => reason), syncAgeMs);
  if (master && report.queue_depth >= MASTER_QUEUE_CAPACITY) return result("QUEUE FULL", "warn", ["Master queue full"]);
  return result(master && report.queue_depth > 0 ? `${report.queue_depth} QUEUED` : "OK", "good", [], syncAgeMs);
}

// master: D 0 telemetry. head: { hseq, firstSeenAt, lastSeenAt } of the latest E line.
export function pipelineHealth({ master, head, connected, now = Date.now() }) {
  if (master && Number.isInteger(master.queue_depth) && master.queue_depth > MASTER_QUEUE_CAPACITY * QUEUE_HEALTH_RATIO) {
    return { ok: false, reason: `Master queue nearly full (${master.queue_depth}/${MASTER_QUEUE_CAPACITY})` };
  }
  if (connected && head && now - head.firstSeenAt > HEAD_STUCK_MS && now - head.lastSeenAt <= STILL_ARRIVING_MS) {
    return { ok: false, reason: `Event #${head.hseq} keeps being retransmitted` };
  }
  return { ok: true, reason: null };
}

// mappings: enabled rows whose role the mode requires [{ node_id, role }]
// telemetry: { [node_id]: normalized telemetry } (node "0" = master)
// masterFresh: the USB link is up and the master has spoken recently
export function wirelessQuality({ mode, mappings, telemetry, masterFresh, connected = false, contract = { ok: true }, pipeline = { ok: true }, now = Date.now() }) {
  const reasons = [];
  if (!masterFresh) reasons.push({ node_id: "0", reason: connected ? "No recent data from master" : "Master not connected" });
  else if (!contract.ok) reasons.push({ node_id: "0", reason: contract.reason });
  if (!pipeline.ok) reasons.push({ node_id: "0", reason: pipeline.reason });

  for (const reason of timingHealth(telemetry?.["0"], { nodeId: "0", now }).reasons) {
    reasons.push({ node_id: "0", reason });
  }

  for (const role of REQUIRED_ROLES[mode] || []) {
    if (!mappings.some((row) => row.role === role)) reasons.push({ node_id: null, role, reason: `No ${role} sensor is mapped` });
  }

  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    for (const reason of timingHealth(telemetry?.[node], { nodeId: node, now }).reasons) {
      reasons.push({ node_id: node, role: mapping.role, reason });
    }
  }
  return { ok: reasons.length === 0, reasons, mappings };
}

export function missingRoles(mode, mappings) {
  const have = new Set(mappings.map((m) => m.role));
  return (REQUIRED_ROLES[mode] || []).filter((r) => !have.has(r));
}
