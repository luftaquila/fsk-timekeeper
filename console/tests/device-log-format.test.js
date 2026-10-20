import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { logCode, logRecord } from "../src/lib/deviceLogFormat";

describe("device log presentation and export", () => {
  it("shows the simulator identity in the real board format", () => {
    assert.equal(logRecord({ kind: "boot", code: "power_on", devid: "F4AEF4AE00000001" }).board, "00000001");
    assert.equal(logRecord({ devid: "unrecognized" }).board, "unrecognized");
  });
  it("identifies cumulative queue retries without presenting them as lost records", () => {
    assert.equal(logRecord({ kind: "X", code: "queue_full", text: "X queue_full 751" }).line,
      "Queue Full Deferrals: 751 Since Boot");
  });
  it("normalizes logged codes without changing source entries", () => {
    for (const [raw, code] of [["USB wake", "usb_wake"], ["power-on", "power_on"],
      ["tel.format", "tel_format"], ["soft reset, CPU lockup", "soft_reset_cpu_lockup"]]) {
      assert.equal(logCode(raw), code);
    }
    const e = { kind: "boot", code: "soft reset", devid: "F37126AAB5D831DD", at: new Date(2026, 9, 15, 21, 2, 49).getTime(), text: "I raw identity" };
    const record = logRecord(e);
    assert.equal(record.board, "B5D831DD");
    assert.match(record.time, /^2026-10-15 /);
    assert.equal(record.line, "Reset: soft reset.");
    assert.deepEqual(Object.keys(record), ["time", "type", "code", "board", "line"]);
    assert.equal(e.text, "I raw identity");
    assert.equal(e.code, "soft reset");
  });
  it("keeps cumulative totals, latest result and unknown event time in concise history", () => {
    const record = logRecord({ kind: "radio", code: "radio_history", historySnapshot: {
      faults: [0, 2, 0, 0, 0, 0], recovery: { count: 1, failures: 0, reason: 2, result: 2 },
    } });
    assert.equal(record.line, "Since Boot\nSPI timeouts: 2\nResets: 1\nFailed: 0\nLast Reason: no master beacon for 10 s\nLast Result: receiver restarted\nEvent Time: Unknown");
  });
  it("preserves saturation and failed recovery", () => {
    assert.equal(logRecord({ kind: "radio", code: "radio_recovery", radioSnapshot: {
      count: 65535, failures: 65535, reason: 3, result: 3,
    } }).line, "Since Boot\nResets: 65535+\nFailed: 65535+\nLast Reason: SPI timeout\nLast Result: failed");
  });
  it("omits raw USB payloads from exported display records", () => {
    const e = { kind: "console", code: "event.unreadable", text: "E malformed payload" };
    assert.equal(logRecord(e).line, "Unreadable event.");
    assert.ok(!JSON.stringify(logRecord(e)).includes("malformed payload"));
    assert.equal(logRecord({ kind: "X", code: "spi_timeout", text: "X spi_timeout 7" }).line, "Count: 7 since boot.");
  });
});
