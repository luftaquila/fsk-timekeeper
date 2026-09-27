import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { wirelessQuality, telemetryAgeMs, missingRoles } from "../src/lib/quality.js";

const NOW = 1_700_000_000_000;
function healthy(node, extra = {}) {
  return {
    node_id: node,
    link_state: "online",
    last_seen_at: NOW - 1000,
    rssi: -60,
    snr: 9,
    skew_ppm: 12,
    beacon_gap: 0,
    sec_drop: 0,
    provisioned: 1,
    sync_valid: 1,
    skew_valid: 1,
    clock_source: "xtal",
    sync_age_ms: 100,
    capture_overflow: 0,
    event_drop: 0,
    queue_depth: 0,
    queue_overflow: 0,
    usb_ref_valid: 1,
    usb_ref_ppm: 25,
    ...extra,
  };
}
const SPRINT = [{ node_id: "A", role: "start" }, { node_id: "B", role: "finish" }];
function gate(mode, mappings, telemetry, masterFresh = true) {
  return wirelessQuality({ mode, mappings, telemetry, masterFresh, now: NOW });
}

describe("wirelessQuality", () => {
  it("passes with a fresh master and healthy mapped sensors", () => {
    const q = gate("sprint", SPRINT, { 0: healthy("0"), A: healthy("A"), B: healthy("B") });
    assert.deepEqual(q, { ok: true, reasons: [], mappings: SPRINT });
  });

  it("fails when the master is disconnected, stale, unprovisioned or on RC", () => {
    assert.match(gate("sprint", SPRINT, { 0: healthy("0"), A: healthy("A"), B: healthy("B") }, false).reasons[0].reason, /not connected/);
    assert.match(gate("sprint", SPRINT, { A: healthy("A"), B: healthy("B") }).reasons[0].reason, /No recent status report from the master/);
    assert.match(gate("sprint", SPRINT, { 0: healthy("0", { last_seen_at: NOW - 12001 }), A: healthy("A"), B: healthy("B") }).reasons[0].reason, /No recent status/);
    const r = gate("sprint", SPRINT, { 0: healthy("0", { provisioned: 0, clock_source: "rc" }), A: healthy("A"), B: healthy("B") }).reasons;
    assert.equal(r.length, 2);
    assert.match(r[0].reason, /HFXO/);
    assert.match(r[1].reason, /radio key/);
  });

  it("requires every role of the mode to be mapped", () => {
    const q = gate("sprint", [{ node_id: "A", role: "start" }], { 0: healthy("0"), A: healthy("A") });
    assert.deepEqual(q.reasons, [{ node_id: null, role: "finish", reason: "No finish sensor is mapped." }]);
    assert.equal(gate("laps", [{ node_id: "A", role: "start" }], { 0: healthy("0"), A: healthy("A") }).ok, true);
    assert.deepEqual(missingRoles("sprint", []), ["start", "finish"]);
    assert.deepEqual(missingRoles("laps", [{ role: "start" }]), []);
    assert.deepEqual(missingRoles("laps", [{ role: "finish" }]), ["start"]);
  });

  it("checks each mapped sensor: freshness, link, key, HFXO, sync, skew", () => {
    const stale = gate("sprint", SPRINT, { 0: healthy("0"), A: healthy("A", { last_seen_at: NOW - 12001 }), B: healthy("B") });
    assert.equal(stale.reasons.length, 1);
    assert.match(stale.reasons[0].reason, /No recent status report from sensor A/);
    assert.equal(stale.reasons[0].role, "start");

    const bad = gate("sprint", SPRINT, {
      0: healthy("0"),
      A: healthy("A", { link_state: "degraded", provisioned: 0, clock_source: "rc", sync_valid: 0, skew_ppm: 101 }),
      B: healthy("B", { sync_age_ms: 7001, skew_valid: 0 }),
    });
    assert.deepEqual(bad.reasons.map((r) => `${r.node_id}:${r.reason.split(" ").slice(-2).join(" ")}`), [
      "A:not healthy.",
      "A:radio key.",
      "A:not confirmed.",
      "A:not valid.",
      "A:not valid.",
      "B:not valid.",
      "B:not valid.",
    ]);
  });

  it("telemetryAgeMs is Infinity without a last_seen_at", () => {
    assert.equal(telemetryAgeMs(null), Infinity);
    assert.equal(telemetryAgeMs({ last_seen_at: NOW - 5 }, NOW), 5);
    assert.equal(telemetryAgeMs({ last_seen_at: NOW + 5 }, NOW), 0);
  });
});
