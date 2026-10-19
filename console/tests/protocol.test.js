import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, formatAck, rowKey, bootIdText, isHexKey, isRadioConfig, radioCommand, radioOf, contractOf, errorNames, resetNames, CONTRACT_SENSOR_ROLE, CONTRACT_VERSION } from "../src/lib/protocol.js";
import { crc32, crc32Hex } from "../src/lib/crc32.js";

const withCrc = (body) => `${body} ${crc32Hex(body)}`;
const C_LINE = withCrc("E 17 0D2243B0 C 1234 7 3735928559 305419896 120 -91.50 9.25 42 2 123456789012345678 123456789028345678");
const L_LINE = withCrc("E 18 0D2243B0 L 1235 7 3735928559 305419896 120 -91.50 9.25 44 45 123456789030000000 123456789031000000");
const K_LINE = withCrc("E 19 0D2243B0 K 1236 7 3735928559 305419896 120 -91.50 9.25 45 123456789040000000 1 2 3 4 5 6 3 1 2 2");
const M_LINE = withCrc("E 20 0 L 0 0 3735928559 3735928559 0 0.00 0.00 0 0 123456789050000000 123456789050000000");
const D_LINE = "D 0D2243B0 OK 12 3 0 4321 -91.50 9.25 22 315 3987 0 1 1 1 XTAL 800 0 0 0 0 5 0 0 4 305419896 3735928559 3 1 2 2 1";
const D_MASTER = "D 0 OK 0 0 0 0 0.00 0.00 0 305 4123 2 1 1 1 XTAL 0 0 0 3 1 256 7 2 0 0 3735928559 0 0 0 0 1";

describe("crc32", () => {
  it("is CRC-32/IEEE", () => {
    assert.equal(crc32("123456789"), 0xcbf43926);
    assert.equal(crc32Hex(""), "00000000");
  });
});

describe("parseLine", () => {
  it("requires an explicit diagnostic validity flag and rejects the old live contract", () => {
    const tokens = D_LINE.split(" ");
    const prefix = tokens.slice(0, 32).join(" ");
    for (const suffix of ["", " 2", " -1", " x", " 1 extra"]) assert.equal(parseLine(prefix + suffix).telemetry, null);
    assert.equal(normalizeTelemetry(parseLine(prefix + " 0").telemetry).diag_valid, 0);
    assert.equal(contractOf(parseLine("I FSK-WL 6 1.0.0 0123456789ABCDEF M 15 922900 7 250.00 16000 4 -9 0")).ok, false);
  });
  it("distinguishes radio clock and initialization faults from MCU HFXO", () => {
    assert.deepEqual(errorNames(0x3020), ["HFXO restart", "radio oscillator / PLL failure", "radio initialization failure"]);
  });
  it("parses USB key readback without accepting malformed or partial keys", () => {
    assert.deepEqual(parseLine(`K ${"AB".repeat(32)}`), { type: "K", key: "ab".repeat(32) });
    assert.deepEqual(parseLine("K NONE"), { type: "K", key: "" });
    for (const line of ["K", "K NONE extra", `K ${"a".repeat(63)}`, `K ${"z".repeat(64)}`]) {
      assert.deepEqual(parseLine(line), { type: "K", key: null });
    }
  });
  it("parses I v7 and checks the contract", () => {
    const i = parseLine("I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 4 -9 0");
    assert.deepEqual(i, { type: "I", product: "FSK-WL", usbProto: 7, fw: "1.0.0", devid: "0123456789ABCDEF", role: "M", radioProto: 17, freqKhz: 922900, sf: 7, bw: 250, ticksPerMs: 16000, resetReason: 4, powerDbm: -9, lbt: 0, v1: false });
    assert.deepEqual(radioOf(i), { freqKhz: 922900, powerDbm: -9, lbt: 0 });
    assert.equal(radioOf(parseLine("I FSK-WL 3 1.0.0 0123456789ABCDEF M 15 921.30 7 250.00 16000 4")), null);
    assert.deepEqual(contractOf(i), { ok: true, reason: null });
    assert.equal(contractOf({ ...i, usbProto: 3 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, role: "S" }).reason, CONTRACT_SENSOR_ROLE);
    assert.equal(contractOf({ ...i, radioProto: 16 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, radioProto: 15 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, radioProto: 11 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, radioProto: 12, usbProto: 2 }).reason, CONTRACT_VERSION);
    assert.equal(contractOf({ ...i, ticksPerMs: 16001 }).reason, CONTRACT_VERSION);
    const v1 = parseLine("I FSK-WL 1.0.0 0123456789ABCDEF 921.30 7 250.00 16000");
    assert.equal(v1.v1, true);
    assert.equal(v1.fw, "1.0.0");
    assert.equal(contractOf(v1).reason, CONTRACT_VERSION);
    assert.equal(contractOf(null).ok, false);
  });

  it("parses H / T / A / X / P v3", () => {
    assert.deepEqual(parseLine("H 18446744073709551615 12345 200 3"), { type: "H", nowTick: "18446744073709551615", uptimeMs: 12345, beaconSeq: 200, nseen: 3 });
    assert.deepEqual(parseLine("T 0123456789abcdef0123456789abcdef 99 7"), { type: "T", requestId: "0123456789abcdef0123456789abcdef", masterTick: "99", masterBootId: "7" });
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

  it("rejects incomplete or invalid recovery summaries", () => {
    const prefix = D_LINE.split(" ").slice(0, 28).join(" ");
    for (const suffix of ["", "1 0 2", "1 2 2 2", "-1 0 2 2", "65536 0 2 2", "1 0 9 2", "1 0 2 4", "0 0 2 2", "1 0 0 0"]) {
      assert.equal(parseLine(`${prefix} ${suffix} 1`).telemetry, null, suffix);
    }
    assert.deepEqual(normalizeTelemetry(parseLine(D_LINE).telemetry).recovery, { count: 3, failures: 1, reason: 2, result: 2 });
    assert.deepEqual(errorNames(1), []); // legacy reset history must never become an active error
  });

  it("parses the 33-token D line", () => {
    const t = normalizeTelemetry(parseLine(D_LINE).telemetry, 1_000_000);
    assert.equal(t.node_id, "0D2243B0");
    assert.equal(t.link_state, "online");
    assert.equal(t.skew_ppm, 12);
    assert.equal(t.last_seen_at, 1_000_000 - 4321);
    assert.equal(t.received_at, 1_000_000);
    assert.equal(t.sync_age_ms, 800);
    assert.equal(t.diag_valid, 1);
    assert.equal(t.err_flags, 5);
    assert.deepEqual(errorNames(t.err_flags), ["SPI timeout"]);
    assert.equal(t.reset_reason, 4);
    assert.deepEqual(resetNames(t.reset_reason), ["soft reset"]);
    assert.deepEqual(resetNames(0), ["power-on"]);
    assert.equal(t.sensor_boot_id, "305419896");
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
  it("requires checkpoint history, bounds counters, and includes it in the CRC", () => {
    const body = "E 1 0D2243B0 K 1 7 1 2 0 0.00 0.00 1 9";
    for (const tail of ["", "0 0 0 0 0 0 0 0 0", "65536 0 0 0 0 0 0 0 0 0", "0 0 0 0 0 0 1 2 2 2", "0 0 0 0 0 0 0 0 2 2"]) {
      assert.equal(parseEventLine(withCrc(`${body} ${tail}`)).stage, "invalid", tail);
    }
    const valid = withCrc(`${body} 65535 2 3 4 5 6 65535 123 8 3`);
    const p = parseEventLine(valid);
    assert.equal(p.stage, "valid");
    assert.deepEqual(p.rows[0].radio_history, { faults: [65535, 2, 3, 4, 5, 6], recovery: { count: 65535, failures: 123, reason: 8, result: 3 } });
    assert.equal(parseEventLine(valid.replace("65535 2", "65534 2")).stage, "crc");
  });
  it("keeps adjacent 64-bit boot IDs distinct through parsing, row keys and ACKs", () => {
    const ids = ["9007199254740992", "9007199254740993", "18446744073709551615"];
    const packets = ids.map((id) => parseEventLine(withCrc(`E 1 0D2243B0 K 1 7 ${id} ${id} 0 0.00 0.00 1 9 0 0 0 0 0 0 0 0 0 0`)));
    assert.equal(new Set(packets.map((p) => rowKey(p.rows[0]))).size, ids.length);
    for (let i = 0; i < ids.length; i++) {
      assert.equal(packets[i].masterBootId, ids[i]);
      assert.equal(packets[i].rows[0].sensor_boot_id, ids[i]);
      assert.equal(formatAck(packets[i]), `C 1 ${ids[i]} ${packets[i].crc}`);
    }
    assert.equal(bootIdText(0xffffffff), "4294967295"); // legacy history
    assert.equal(bootIdText("00000000000000000000"), "0");
    for (const id of ["18446744073709551616", "-1", "1e3", 9007199254740992]) {
      assert.equal(bootIdText(id), null);
    }
    assert.equal(parseLine("T token 1 18446744073709551616").masterBootId, null);
    assert.equal(parseEventLine(withCrc("E 1 0D2243B0 K 1 7 18446744073709551616 1 0 0.00 0.00 1 9")).reason, "master_boot_id");
    assert.equal(parseEventLine(withCrc("E 1 0D2243B0 K 1 7 1 18446744073709551616 0 0.00 0.00 1 9")).reason, "sensor_boot_id");
  });

  it("expands a capture bundle into one row per edge", () => {
    const p = parseEventLine(C_LINE);
    assert.equal(p.stage, "valid");
    assert.equal(p.hseq, 17);
    assert.equal(p.masterBootId, "3735928559");
    assert.equal(p.rows.length, 2);
    assert.deepEqual(
      p.rows.map((r) => [r.kind, r.capture_seq, r.master_tick, r.end_tick]),
      [
        ["capture", 42, "123456789012345678", "123456789012345678"],
        ["capture", 43, "123456789028345678", "123456789028345678"],
      ],
    );
    assert.equal(p.rows[0].raw, C_LINE);
    assert.equal(p.rows[0].usb_proto, 7);
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
    assert.equal(bad.masterBootId, "1");
    assert.equal(invalid("E 5 0D2243B0 K 1 7 x 2 0 0.00 0.00 1 9").masterBootId, null);
  });

  it("validates key material", () => {
    assert.equal(isHexKey("a".repeat(64)), true);
    assert.equal(isHexKey("a".repeat(63)), false);
    assert.equal(isHexKey("g".repeat(64)), false);
  });
});

describe("radio settings", () => {
  it("accepts the Ra-01SH range only and formats R", () => {
    assert.ok(isRadioConfig({ freqKhz: 803000, powerDbm: -9, lbt: 0 }));
    assert.ok(isRadioConfig({ freqKhz: 930000, powerDbm: 22, lbt: 1 }));
    for (const bad of [{ freqKhz: 802999, powerDbm: 0, lbt: 1 }, { freqKhz: 930001, powerDbm: 0, lbt: 1 }, { freqKhz: 922900, powerDbm: 23, lbt: 1 },
      { freqKhz: 922900, powerDbm: -10, lbt: 1 }, { freqKhz: 922900, powerDbm: 0, lbt: 2 }, { freqKhz: 922900.5, powerDbm: 0, lbt: 1 }, null]) {
      assert.equal(isRadioConfig(bad), false, JSON.stringify(bad));
    }
    assert.equal(radioCommand({ freqKhz: 922900, powerDbm: -3, lbt: 1 }), "R 922900 -3 1");
  });
});

describe("admission contract", () => {
  it("parses only full sensor IDs and defined states, and rejects the prior live protocol", () => {
    for (const state of ["WAIT", "SYNC", "READY"]) {
      assert.deepEqual(parseLine(`J 1234ABCD ${state}`), { type: "J", node: "1234ABCD", state });
    }
    for (const line of ["J 0 READY", "J 00000000 READY", "J 1234ABCD YES", "J 1234ABCD READY extra", "J 1234ABCD REMOVED"]) assert.equal(parseLine(line), null);
    assert.equal(contractOf(parseLine("I FSK-WL 5 v1.0.0 0123456789ABCDEF M 14 922900 7 250.00 16000 0 12 1")).ok, false);
  });
});
