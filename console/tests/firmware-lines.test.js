// USB v8 contract on both sides: device/firmware/tests/usb_lines.txt holds firmware-formatted lines
// with extreme values (`make test` checks them); the console parses the same file.
import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, contractOf, formatAck, errorNames, resetNames, radioOf, cameraNodeOf } from "../src/lib/protocol.js";
import { lineTick } from "../src/lib/camera.js";

const LINES = readFileSync(resolve(__dirname, "../../device/firmware/tests/usb_lines.txt"), "utf8").trimEnd().split("\n");

describe("firmware-formatted lines", () => {
  it("I: the contract accepts a master and names a sensor-role board", () => {
    const m = parseLine(LINES[0]);
    assert.equal(m.usbProto, 8);
    assert.deepEqual(radioOf(m), { freqKhz: 922900, powerDbm: 12, lbt: 1 });
    assert.deepEqual(radioOf(parseLine(LINES[1])), { freqKhz: 803000, powerDbm: -9, lbt: 0 });
    assert.equal(m.radioProto, 17);
    assert.equal(m.devid, "0123ABCD89EF4567");
    assert.equal(m.role, "M");
    assert.equal(m.ticksPerMs, 16000);
    assert.deepEqual(resetNames(m.resetReason), ["reset pin", "fault reboot"]);
    assert.deepEqual(contractOf(m), { ok: true, reason: null });
    assert.match(contractOf(parseLine(LINES[1])).reason, /Board is not a master/);
  });

  it("E: every kind parses, crc included, and acks echo hseq / boot / crc", () => {
    const c = parseEventLine(LINES[3]);
    assert.equal(c.stage, "valid");
    assert.equal(c.rows.length, 5);
    assert.deepEqual(c.rows.map((r) => r.capture_seq), [4294967294, 4294967295, 0, 1, 2]); // seq wraps
    assert.equal(c.rows[4].master_tick, "18446744073709400006");
    assert.equal(c.rows[0].flags, 15);
    assert.equal(c.rows[0].master_boot_id, "18446744073709551615");
    assert.equal(c.rows[0].sensor_boot_id, "18446744073709551615");
    assert.equal(formatAck(c), `C 1 18446744073709551615 ${LINES[3].slice(-8)}`);

    const unknown = parseEventLine(LINES[4]);
    assert.equal(unknown.stage, "valid");
    assert.equal(unknown.rows[0].kind, "loss");
    assert.equal(unknown.rows[0].end_seq, 3);
    assert.equal(parseEventLine(LINES[5]).rows[0].end_tick, "9000");
    assert.equal(parseEventLine(LINES[6]).rows[0].kind, "checkpoint");
    assert.deepEqual(parseEventLine(LINES[6]).rows[0].radio_history, {
      faults: [65535, 65535, 65535, 65535, 65535, 65535],
      recovery: { count: 65535, failures: 123, reason: 2, result: 2 },
    });
    const end = parseEventLine(LINES[7]);
    assert.equal(end.stage, "valid");
    assert.equal(end.node, "0");
    assert.equal(end.rows[0].master_tick, "5000");
    // one flipped character is a crc mismatch, never acked
    assert.equal(parseEventLine(LINES[3].replace("C 65535", "C 65534")).stage, "crc");
  });

  it("E: camera node records (V crossings, bounded L, K without history)", () => {
    const camera = cameraNodeOf(parseLine(LINES[0]));
    assert.equal(camera, "89EF4567");
    const max = parseEventLine(LINES[8]);
    assert.equal(max.stage, "valid");
    assert.equal(max.node, camera);
    assert.deepEqual(max.rows[0].camera, { line: "18446744073709551614", period: 16000, run_lo: "18446744073709551000",
      run_hi: "18446744073709551615", car: 999, conf: 100 });
    assert.equal(max.rows[0].kind, "capture");
    assert.equal(max.rows[0].capture_seq, 4294967295);
    const v = parseEventLine(LINES[9]);
    assert.equal(v.rows[0].sensor_boot_id, "42");
    assert.equal(v.rows[0].master_tick, "16480400");
    assert.equal(lineTick(v.rows[0], 1200), 16480400n - 276n);
    assert.equal(lineTick(v.rows[0], 640), null); // line 640 - 1 lies outside the run
    assert.equal(lineTick(v.rows[0], 1501), null);
    const loss = parseEventLine(LINES[10]).rows[0];
    assert.deepEqual([loss.kind, loss.capture_seq, loss.end_seq, loss.master_tick, loss.end_tick], ["loss", 2, 2, "16500001", "16600000"]);
    const k = parseEventLine(LINES[11]).rows[0];
    assert.equal(k.kind, "checkpoint");
    assert.deepEqual(k.radio_history, { faults: [0, 0, 0, 0, 0, 0], recovery: { count: 0, failures: 0, reason: 0, result: 0 } });
    // A crossing line outside its run is a broken contract.
    assert.equal(parseEventLine(LINES[9].replace(" 640 1500 ", " 1201 1500 ")).stage, "crc");
  });

  it("D, P, T, A, X", () => {
    const s = normalizeTelemetry(parseLine(LINES[12]).telemetry, 1000);
    assert.equal(s.link_state, "degraded");
    assert.equal(s.skew_ppm, -32768);
    assert.equal(s.sync_valid, 1);
    assert.equal(s.skew_valid, 0);
    assert.equal(s.fifo_drop, 2);
    assert.equal(s.diag_valid, 1);
    assert.deepEqual(errorNames(s.err_flags), ["ACK anomaly"]);
    assert.equal(s.reset_reason, 64);
    assert.equal(s.sensor_boot_id, "9007199254740993");
    const m = normalizeTelemetry(parseLine(LINES[13]).telemetry, 1000);
    assert.equal(m.queue_depth, 3);
    assert.equal(m.diag_valid, 1);
    assert.equal(m.ver_drop, 5);
    assert.equal(m.tx_drop, 9);
    const cam = normalizeTelemetry(parseLine(LINES[14]).telemetry, 1000);
    assert.deepEqual([cam.node_id, cam.link_state, cam.sync_valid, cam.skew_valid, cam.clock_source, cam.latency_ms,
      cam.capture_overflow, cam.fifo_drop, cam.sensor_boot_id, cam.master_boot_id, cam.provisioned, cam.diag_valid],
      ["89EF4567", "online", 1, 1, "xtal", 412, 2, 1, "42", "11", 1, 1]);
    const p = normalizePps(parseLine(LINES[15]).pps, 1);
    assert.equal(p.seg, 3);
    assert.equal(p.n, 17);
    assert.equal(p.ppb, -1234);
    assert.equal(parseLine(LINES[16]).masterBootId, "18446744073709551615");
    assert.equal(parseLine(LINES[17]).cmd, "CP");
    assert.equal(parseLine(LINES[18]).cmd, "V");
    assert.equal(parseLine(LINES[19]).reason, "camwait");
    assert.deepEqual(parseLine(LINES[20]).args, ["3"]);
    assert.deepEqual(parseLine(LINES[21]).args, ["00027F38", "0002A001", "hard", "00000400"]);
  });
});
