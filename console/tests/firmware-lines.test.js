// The USB v2 contract on both sides: device/firmware/tests/usb_lines.txt holds lines printed
// by the firmware's own formatters with extreme values (`make test` checks the file against
// them), and the console parses the same file.
import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, contractOf, formatAck, errorNames, resetNames } from "../src/lib/protocol.js";

const LINES = readFileSync(resolve(__dirname, "../../device/firmware/tests/usb_lines.txt"), "utf8").trimEnd().split("\n");

describe("firmware-formatted lines", () => {
  it("I: the contract accepts a master and names a sensor-role board", () => {
    const m = parseLine(LINES[0]);
    assert.equal(m.usbProto, 2);
    assert.equal(m.radioProto, 11);
    assert.equal(m.devid, "0123ABCD89EF4567");
    assert.equal(m.role, "M");
    assert.equal(m.ticksPerMs, 16000);
    assert.deepEqual(resetNames(m.resetReason), ["reset pin", "fault reboot"]);
    assert.deepEqual(contractOf(m), { ok: true, reason: null });
    assert.match(contractOf(parseLine(LINES[1])).reason, /booted as a sensor/);
  });

  it("E: every kind parses, crc included, and acks echo hseq / boot / crc", () => {
    const c = parseEventLine(LINES[3]);
    assert.equal(c.stage, "valid");
    assert.equal(c.rows.length, 5);
    assert.deepEqual(c.rows.map((r) => r.capture_seq), [4294967294, 4294967295, 0, 1, 2]); // seq wraps
    assert.equal(c.rows[4].master_tick, "18446744073709400006");
    assert.equal(c.rows[0].flags, 15);
    assert.equal(formatAck(c), "C 1 4294967295 AADCA20A");

    const unknown = parseEventLine(LINES[4]);
    assert.equal(unknown.stage, "valid");
    assert.equal(unknown.rows[0].kind, "loss");
    assert.equal(unknown.rows[0].end_seq, 3);
    assert.equal(parseEventLine(LINES[5]).rows[0].end_tick, "9000");
    assert.equal(parseEventLine(LINES[6]).rows[0].kind, "checkpoint");
    const end = parseEventLine(LINES[7]);
    assert.equal(end.stage, "valid");
    assert.equal(end.node, "0");
    assert.equal(end.rows[0].master_tick, "5000");
    // one flipped character is a crc mismatch, never acked
    assert.equal(parseEventLine(LINES[3].replace("C 65535", "C 65534")).stage, "crc");
  });

  it("D, P, T, A, X", () => {
    const s = normalizeTelemetry(parseLine(LINES[8]).telemetry, 1000);
    assert.equal(s.link_state, "degraded");
    assert.equal(s.skew_ppm, -32768);
    assert.equal(s.sync_valid, 1);
    assert.equal(s.skew_valid, 0);
    assert.equal(s.fifo_drop, 2);
    assert.deepEqual(errorNames(s.err_flags), ["radio reset", "ACK anomaly"]);
    assert.equal(s.reset_reason, 64);
    const m = normalizeTelemetry(parseLine(LINES[9]).telemetry, 1000);
    assert.equal(m.queue_depth, 3);
    assert.equal(m.ver_drop, 5);
    assert.equal(m.tx_drop, 9);
    const p = normalizePps(parseLine(LINES[10]).pps, 1);
    assert.equal(p.seg, 3);
    assert.equal(p.n, 17);
    assert.equal(p.ppb, -1234);
    assert.equal(parseLine(LINES[11]).masterBootId, 7);
    assert.equal(parseLine(LINES[12]).cmd, "CP");
    assert.deepEqual(parseLine(LINES[13]).args, ["3"]);
    assert.deepEqual(parseLine(LINES[14]).args, ["00027F38", "0002A001", "hard", "00000400"]);
  });
});
