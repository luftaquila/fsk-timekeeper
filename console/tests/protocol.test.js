import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { parseLine, validateEvent, normalizeTelemetry, eventKey, formatAck, isHexKey } from "../src/lib/protocol.js";

// Literal lines in the shape proto_usb.c emits.
const E_LINE = "E 0D2243B0 1234 123456789012345678 15 -91.50 9.25 3735928559 305419896 42 42 123456789012345678 120";
const E_CHECKPOINT = "E 0D2243B0 1235 123456789012400000 47 -91.50 9.25 3735928559 305419896 42 42 123456789012400000 120";
const E_LOSS = "E 0D2243B0 1236 123456789012500000 31 -91.50 9.25 3735928559 305419896 43 45 123456789012600000 120";
const D_LINE = "D 0D2243B0 OK -1234567 12 3 0 4321 -91.50 9.25 22 315 3987 0 1 1 1 XTAL 800 0 0 0 0 0 0 305419896 3735928559";
const D_MASTER = "D 0 OK 0 0 0 0 0 0.00 0.00 0 305 4123 2 1 1 1 XTAL 0 0 0 3 1 1 25 0 3735928559";

describe("parseLine", () => {
  it("parses I / H / T / A / X", () => {
    assert.deepEqual(parseLine("I FSK-WL 1.0.0 0123456789ABCDEF 921.30 7 250.00 16000"), {
      type: "I", product: "FSK-WL", fw: "1.0.0", devid: "0123456789ABCDEF", freqMhz: 921.3, sf: 7, bw: 250, ticksPerMs: 16000,
    });
    assert.deepEqual(parseLine("H 18446744073709551615 12345 200 3"), {
      type: "H", nowTick: "18446744073709551615", uptimeMs: 12345, beaconSeq: 200, nseen: 3,
    });
    assert.deepEqual(parseLine("T 0123456789abcdef0123456789abcdef 99 7"), {
      type: "T", requestId: "0123456789abcdef0123456789abcdef", masterTick: "99", masterBootId: 7,
    });
    assert.deepEqual(parseLine("A K OK\r"), { type: "A", cmd: "K" });
    assert.deepEqual(parseLine("X noprov"), { type: "X", reason: "noprov" });
    assert.equal(parseLine("   "), null);
    assert.equal(parseLine("Z foo").type, "?");
  });

  it("parses E with BigInt-safe ticks and upper-cased node ids", () => {
    const { event } = parseLine(E_LINE.replace("0D2243B0", "0d2243b0"));
    assert.equal(event.node_id, "0D2243B0");
    assert.equal(event.ev_seq, 1234);
    assert.equal(event.master_tick, "123456789012345678");
    assert.equal(event.flags, 15);
    assert.equal(event.rssi, -91.5);
    assert.equal(event.snr, 9.25);
    assert.equal(event.master_boot_id, 3735928559);
    assert.equal(event.sensor_boot_id, 305419896);
    assert.equal(event.capture_seq, 42);
    assert.equal(event.end_seq, 42);
    assert.equal(event.end_tick, "123456789012345678");
    assert.equal(event.sync_age_ms, 120);
  });

  it("parses the 27-token D line", () => {
    const { telemetry } = parseLine(D_LINE);
    assert.equal(telemetry.node_id, "0D2243B0");
    assert.equal(telemetry.link_state, "online");
    assert.equal(telemetry.offset_us, Math.round(-1234567 / 16));
    assert.equal(telemetry.skew_ppm, 12);
    assert.equal(telemetry.rx_miss, 3);
    assert.equal(telemetry.beacon_gap, 0);
    assert.equal(telemetry.last_seen_ms, 4321);
    assert.equal(telemetry.rssi, -91.5);
    assert.equal(telemetry.latency_ms, 22);
    assert.equal(telemetry.temp_c10, 315);
    assert.equal(telemetry.batt_mv, 3987);
    assert.equal(telemetry.provisioned, 1);
    assert.equal(telemetry.clock_source, "xtal");
    assert.equal(telemetry.sync_age_ms, 800);
    assert.equal(telemetry.sensor_boot_id, 305419896);
    assert.equal(telemetry.master_boot_id, 3735928559);
    assert.equal(parseLine(D_LINE.replace(" OK ", " STALE ")).telemetry.link_state, "degraded");
    assert.equal(parseLine(D_LINE.replace(" OK ", " LOST ")).telemetry.link_state, "lost");
    const master = parseLine(D_MASTER).telemetry;
    assert.equal(master.node_id, "0");
    assert.equal(master.queue_depth, 3);
    assert.equal(master.queue_overflow, 1);
    assert.equal(master.usb_ref_valid, 1);
    assert.equal(master.usb_ref_ppm, 25);
    assert.equal(master.sec_drop, 2);
  });
});

describe("validateEvent", () => {
  const base = () => parseLine(E_LINE).event;

  it("accepts a healthy capture, a checkpoint and a loss range", () => {
    assert.equal(validateEvent(base()).ok, true);
    assert.equal(validateEvent(parseLine(E_CHECKPOINT).event).ok, true);
    assert.equal(validateEvent(parseLine(E_LOSS).event).ok, true);
    assert.equal(validateEvent(base()).row.master_tick, "123456789012345678");
  });

  it("rejects malformed keys and evidence", () => {
    assert.equal(validateEvent({ ...base(), node_id: "" }).reason, "node_id");
    assert.equal(validateEvent({ ...base(), master_tick: "x" }).reason, "master_tick");
    assert.equal(validateEvent({ ...base(), master_tick: "18446744073709551616" }).reason, "master_tick");
    assert.equal(validateEvent({ ...base(), ev_seq: 65536 }).reason, "ev_seq");
    assert.equal(validateEvent({ ...base(), master_boot_id: -1 }).reason, "master_boot_id");
    assert.equal(validateEvent({ ...base(), flags: 128 }).reason, "capture_evidence");
    assert.equal(validateEvent({ ...base(), sync_age_ms: 70000 }).reason, "capture_evidence");
    // non-loss must have end_seq == capture_seq and end_tick == tick
    assert.equal(validateEvent({ ...base(), end_seq: 43 }).reason, "capture_evidence");
    assert.equal(validateEvent({ ...base(), end_tick: "1" }).reason, "capture_evidence");
    // loss + checkpoint is contradictory
    assert.equal(validateEvent({ ...base(), flags: 48 }).reason, "capture_evidence");
    // range wider than 2^31
    assert.equal(validateEvent({ ...base(), flags: 31, end_seq: 42 + 0x80000000 }).reason, "capture_evidence");
    assert.equal(validateEvent({ ...base(), capture_seq: NaN }).reason, "capture_evidence");
  });
});

describe("normalizeTelemetry", () => {
  it("converts last_seen_ms into an absolute last_seen_at", () => {
    const t = normalizeTelemetry(parseLine(D_LINE).telemetry, 1_000_000);
    assert.equal(t.last_seen_at, 1_000_000 - 4321);
    assert.equal(t.received_at, 1_000_000);
    assert.equal(t.link_state, "online");
    assert.equal(t.provisioned, 1);
  });
  it("nulls out-of-contract values", () => {
    const t = normalizeTelemetry({ node_id: "A", provisioned: 2, clock_source: "weird", sync_age_ms: -5, last_seen_ms: NaN }, 5);
    assert.equal(t.provisioned, null);
    assert.equal(t.clock_source, null);
    assert.equal(t.sync_age_ms, 0);
    assert.equal(t.last_seen_at, 5);
    assert.equal(normalizeTelemetry({ node_id: "" }), null);
  });
});

describe("ack + keys", () => {
  it("formats the C line from the stored row and keys the dedupe tuple", () => {
    const { row } = validateEvent(parseLine(E_LINE).event);
    assert.equal(formatAck(row), "C 0D2243B0 1234 123456789012345678 3735928559 305419896");
    assert.equal(eventKey(row), "0D2243B0:1234:123456789012345678:3735928559:305419896");
  });
  it("validates key material", () => {
    assert.equal(isHexKey("a".repeat(64)), true);
    assert.equal(isHexKey("a".repeat(63)), false);
    assert.equal(isHexKey("g".repeat(64)), false);
  });
});
