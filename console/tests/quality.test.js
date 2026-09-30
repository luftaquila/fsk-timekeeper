import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { wirelessQuality, telemetryAgeMs, missingRoles, pipelineHealth } from "../src/lib/quality.js";

const NOW = 1_700_000_000_000;
function healthy(node, extra = {}) {
  return {
    node_id: node,
    link_state: "online",
    received_at: NOW - 1000,
    last_seen_at: NOW - 30000, // informational only: the firmware state decides
    rssi: -60,
    snr: 9,
    skew_ppm: 12,
    provisioned: 1,
    sync_valid: 1,
    skew_valid: 1,
    clock_source: "xtal",
    sync_age_ms: 9000, // no separate age check: the sync_valid flag decides
    queue_depth: 0,
    ...extra,
  };
}
const SPRINT = [
  { node_id: "A", role: "start" },
  { node_id: "B", role: "finish" },
];
function gate(mode, mappings, telemetry, { masterFresh = true, contract, pipeline } = {}) {
  return wirelessQuality({ mode, mappings, telemetry, masterFresh, contract, pipeline, now: NOW });
}
const ALL = () => ({ 0: healthy("0"), A: healthy("A"), B: healthy("B") });

describe("wirelessQuality", () => {
  it("passes with a fresh master and healthy mapped sensors", () => {
    assert.deepEqual(gate("sprint", SPRINT, ALL()), { ok: true, reasons: [], mappings: SPRINT });
  });

  it("fails on the master link, the contract and the pipeline", () => {
    assert.match(gate("sprint", SPRINT, ALL(), { masterFresh: false }).reasons[0].reason, /not connected/);
    assert.match(gate("sprint", SPRINT, ALL(), { contract: { ok: false, reason: "Firmware and console versions do not match — update both." } }).reasons[0].reason, /versions do not match/);
    assert.match(gate("sprint", SPRINT, ALL(), { pipeline: { ok: false, reason: "queue full" } }).reasons[0].reason, /queue full/);
    const t = ALL();
    t[0] = healthy("0", { received_at: NOW - 12001 });
    assert.match(gate("sprint", SPRINT, t).reasons[0].reason, /No recent status report from the master/);
    const r = gate("sprint", SPRINT, { ...ALL(), 0: healthy("0", { provisioned: 0, clock_source: "rc" }) }).reasons;
    assert.deepEqual(r.map((x) => x.reason.split(" ").slice(-3).join(" ")), ["(HFXO) is not confirmed.", "has no radio key."].map((s) => s.split(" ").slice(-3).join(" ")));
  });

  it("requires every role of the mode to be mapped", () => {
    const q = gate("sprint", [{ node_id: "A", role: "start" }], { 0: healthy("0"), A: healthy("A") });
    assert.deepEqual(q.reasons, [{ node_id: null, role: "finish", reason: "No finish sensor is mapped." }]);
    assert.equal(gate("laps", [{ node_id: "A", role: "start" }], { 0: healthy("0"), A: healthy("A") }).ok, true);
    assert.deepEqual(missingRoles("sprint", []), ["start", "finish"]);
    assert.deepEqual(missingRoles("laps", [{ role: "finish" }]), ["start"]);
  });

  it("uses the firmware link state and the age of the D line", () => {
    const stale = gate("sprint", SPRINT, { ...ALL(), A: healthy("A", { received_at: NOW - 12001 }) });
    assert.equal(stale.reasons.length, 1);
    assert.match(stale.reasons[0].reason, /No recent status report from sensor A/);
    const bad = gate("sprint", SPRINT, {
      0: healthy("0"),
      A: healthy("A", { link_state: "degraded", provisioned: 0, clock_source: "rc", sync_valid: 0, skew_ppm: 101 }),
      B: healthy("B", { skew_valid: 0 }),
    });
    assert.deepEqual(
      bad.reasons.map((r) => `${r.node_id}:${r.reason.split(" ").slice(-2).join(" ")}`),
      ["A:not healthy.", "A:radio key.", "A:not confirmed.", "A:not valid.", "A:not valid.", "B:not valid."],
    );
  });

  it("telemetryAgeMs counts from the report's arrival", () => {
    assert.equal(telemetryAgeMs(null), Infinity);
    assert.equal(telemetryAgeMs({ received_at: NOW - 5 }, NOW), 5);
    assert.equal(telemetryAgeMs({ received_at: NOW + 5 }, NOW), 0);
  });
});

describe("pipelineHealth", () => {
  it("refuses a nearly full host queue and an event that keeps coming back", () => {
    assert.equal(pipelineHealth({ master: { queue_depth: 12 }, connected: true, now: NOW }).ok, true);
    assert.match(pipelineHealth({ master: { queue_depth: 13 }, connected: true, now: NOW }).reason, /nearly full \(13\/16\)/);
    const head = { hseq: 7, firstSeenAt: NOW - 5001, lastSeenAt: NOW - 100 };
    assert.match(pipelineHealth({ master: null, head, connected: true, now: NOW }).reason, /#7/);
    assert.equal(pipelineHealth({ head: { ...head, lastSeenAt: NOW - 3000 }, connected: true, now: NOW }).ok, true); // acked long ago
    assert.equal(pipelineHealth({ head: { ...head, firstSeenAt: NOW - 4000 }, connected: true, now: NOW }).ok, true);
    assert.equal(pipelineHealth({ head, connected: false, now: NOW }).ok, true);
  });
});
