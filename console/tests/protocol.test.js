import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, formatAck, rowKey, isHexKey, contractOf, errorNames, resetNames, CONTRACT_SENSOR_ROLE, CONTRACT_VERSION } from "../src/lib/protocol.js";
import { crc32, crc32Hex } from "../src/lib/crc32.js";

const withCrc = (body) => `${body} ${crc32Hex(body)}`;
const C_LINE = withCrc("E 17 0D2243B0 C 1234 7 3735928559 305419896 120 -91.50 9.25 42 2 123456789012345678 123456789028345678");
const L_LINE = withCrc("E 18 0D2243B0 L 1235 7 3735928559 305419896 120 -91.50 9.25 44 45 123456789030000000 123456789031000000");
const K_LINE = withCrc("E 19 0D2243B0 K 1236 7 3735928559 305419896 120 -91.50 9.25 45 123456789040000000");
const M_LINE = withCrc("E 20 0 L 0 0 3735928559 3735928559 0 0.00 0.00 0 0 123456789050000000 123456789050000000");
const D_LINE = "D 0D2243B0 OK 12 3 0 4321 -91.50 9.25 22 315 3987 0 1 1 1 XTAL 800 0 0 0 0 5 0 0 4 305419896 3735928559";
const D_MASTER = "D 0 OK 0 0 0 0 0.00 0.00 0 305 4123 2 1 1 1 XTAL 0 0 0 3 1 256 7 2 0 0 3735928559";

describe("crc32", () => {
  it("is CRC-32/IEEE", () => {
    assert.equal(crc32("123456789"), 0xcbf43926);
    assert.equal(crc32Hex(""), "00000000");
  });
});

describe("parseLine", () => {
  it("parses I v2 and checks the contract", () => {
    const i = parseLine("I FSK-WL 2 1.0.0 0123456789ABCDEF M 11 921.30 7 250.00 16000 4");
    assert.deepEqual(i, { type: "I", product: "FSK-WL", usbProto: 2, fw: "1.0.0", devid: "0123456789ABCDEF", role: "M", radioProto: 11, freqMhz: 921.3, sf: 7, bw: 250, ticksPerMs: 16000, resetReason: 4, v1: false });
    assert.deepEqual(contractOf(i), { ok: true, reason: null });
    assert.equal(contractOf({ ...i, role: "S" }).reason, CONTRACT_SENSOR_ROLE);
    assert.equal(contractOf({ ...i, radioProto: 10 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, ticksPerMs: 16001 }).reason, CONTRACT_VERSION);
    const v1 = parseLine("I FSK-WL 1.0.0 0123456789ABCDEF 921.30 7 250.00 16000");
    assert.equal(v1.v1, true);
    assert.equal(v1.fw, "1.0.0");
    assert.equal(contractOf(v1).reason, CONTRACT_VERSION);
    assert.equal(contractOf(null).ok, false);
  });

  it("parses H / T / A / X / P v2", () => {
    assert.deepEqual(parseLine("H 18446744073709551615 12345 200 3"), { type: "H", nowTick: "18446744073709551615", uptimeMs: 12345, beaconSeq: 200, nseen: 3 });
    assert.deepEqual(parseLine("T 0123456789abcdef0123456789abcdef 99 7"), { type: "T", requestId: "0123456789abcdef0123456789abcdef", masterTick: "99", masterBootId: 7 });
    assert.deepEqual(parseLine("A K OK\r"), { type: "A", cmd: "K" });
    assert.deepEqual(parseLine("X radio_reset 3"), { type: "X", reason: "radio_reset", args: ["3"], text: "X radio_reset 3" });
    const p = normalizePps(parseLine("P 123456789012345678 1727500000 12345 1 1 9 64 3 17").pps, 5000);
    assert.deepEqual(p, { tick: "123456789012345678", utc: 1727500000, ppb: 12345, valid: 1, fix: 1, sats: 9, span: 64, seg: 3, n: 17, at: 5000 });
    const unq = normalizePps(parseLine("P 5 0 0 0 0 0 0 0 9").pps, 1);
    assert.equal(unq.seg, 0);
    assert.equal(unq.n, 0);
    assert.equal(parseLine("P 1 2 3").pps, null);
    assert.equal(parseLine("   "), null);
    assert.equal(parseLine("Z foo").type, "?");
  });

  it("parses the 28-token D line", () => {
    const t = normalizeTelemetry(parseLine(D_LINE).telemetry, 1_000_000);
    assert.equal(t.node_id, "0D2243B0");
    assert.equal(t.link_state, "online");
    assert.equal(t.skew_ppm, 12);
    assert.equal(t.last_seen_at, 1_000_000 - 4321);
    assert.equal(t.received_at, 1_000_000);
    assert.equal(t.sync_age_ms, 800);
    assert.equal(t.err_flags, 5);
    assert.deepEqual(errorNames(t.err_flags), ["radio reset", "SPI timeout"]);
    assert.equal(t.reset_reason, 4);
    assert.deepEqual(resetNames(t.reset_reason), ["soft reset"]);
    assert.deepEqual(resetNames(0), ["power-on"]);
    assert.equal(t.sensor_boot_id, 305419896);
    const m = normalizeTelemetry(parseLine(D_MASTER).telemetry, 1);
    assert.equal(m.queue_depth, 3);
    assert.equal(m.queue_overflow, 1);
    assert.equal(m.err_flags, 256);
    assert.equal(m.ver_drop, 7);
    assert.equal(m.tx_drop, 2);
    assert.equal(m.sec_drop, 2);
    assert.equal(parseLine("D 0 OK 1 2 3").telemetry, null);
    assert.equal(parseLine(D_LINE.replace(" OK ", " STALE ")).telemetry.link_state, "degraded");
  });
});

describe("parseEventLine", () => {
  it("expands a capture bundle into one row per edge", () => {
    const p = parseEventLine(C_LINE);
    assert.equal(p.stage, "valid");
    assert.equal(p.hseq, 17);
    assert.equal(p.masterBootId, 3735928559);
    assert.equal(p.rows.length, 2);
    assert.deepEqual(
      p.rows.map((r) => [r.kind, r.capture_seq, r.master_tick, r.end_tick]),
      [
        ["capture", 42, "123456789012345678", "123456789012345678"],
        ["capture", 43, "123456789028345678", "123456789028345678"],
      ],
    );
    assert.equal(p.rows[0].raw, C_LINE);
    assert.equal(p.rows[0].usb_proto, 2);
    assert.equal(rowKey(p.rows[1]), "3735928559:0D2243B0:305419896:capture:43:123456789028345678");
    assert.equal(formatAck(p), `C 17 3735928559 ${C_LINE.slice(-8)}`);
  });

  it("reads loss, checkpoint and master timebase-end lines", () => {
    const l = parseEventLine(L_LINE).rows[0];
    assert.deepEqual([l.kind, l.capture_seq, l.end_seq, l.master_tick, l.end_tick], ["loss", 44, 45, "123456789030000000", "123456789031000000"]);
    const k = parseEventLine(K_LINE).rows[0];
    assert.deepEqual([k.kind, k.capture_seq, k.end_seq, k.master_tick], ["checkpoint", 45, 45, "123456789040000000"]);
    const m = parseEventLine(M_LINE);
    assert.equal(m.stage, "valid");
    assert.equal(m.rows[0].node_id, "0");
  });

  it("classifies unreadable, corrupted and contract-breaking lines", () => {
    assert.equal(parseEventLine("E x 0D2243B0 C").stage, "unreadable");
    assert.equal(parseEventLine(C_LINE.slice(0, -1)).stage, "unreadable");
    const corrupt = C_LINE.replace("0D2243B0", "0D2243B1");
    assert.deepEqual([parseEventLine(corrupt).stage, parseEventLine(corrupt).hseq], ["crc", 17]);
    const invalid = (body) => parseEventLine(withCrc(body));
    assert.equal(invalid("E 5 0D2243B0 Z 1 7 1 2 0 0.00 0.00 1 1 5").reason, "kind");
    assert.equal(invalid("E 5 0D2243B0 C 1 7 1 2 0 0.00 0.00 1 6 1 2 3 4 5 6").reason, "edge count");
    assert.equal(invalid("E 5 0D2243B0 C 1 7 1 2 0 0.00 0.00 1 2 9 9").reason, "edge order");
    assert.equal(invalid("E 5 0D2243B0 C 1 23 1 2 0 0.00 0.00 1 1 9").reason, "flags");
    assert.equal(invalid("E 5 0 C 0 0 1 1 0 0.00 0.00 0 1 9").reason, "master record");
    assert.equal(invalid("E 5 0D2243B0 L 1 7 1 2 0 0.00 0.00 5 4 9 1").reason, "loss range");
    const bad = invalid("E 5 0D2243B0 K 1 7 1 2 0 0.00 0.00 1 x");
    assert.equal(bad.stage, "invalid");
    assert.equal(bad.node, "0D2243B0");
    assert.equal(bad.masterBootId, 1);
    assert.equal(invalid("E 5 0D2243B0 K 1 7 x 2 0 0.00 0.00 1 9").masterBootId, null);
  });

  it("validates key material", () => {
    assert.equal(isHexKey("a".repeat(64)), true);
    assert.equal(isHexKey("a".repeat(63)), false);
    assert.equal(isHexKey("g".repeat(64)), false);
  });
});
