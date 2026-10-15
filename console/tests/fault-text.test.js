import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { formatFaultReason, faultWindow } from "../src/lib/fault-text";

describe("saved fault presentation", () => {
  it("normalizes the six legacy sensor causes without changing unknown evidence", () => {
    const cases = [
      ["lost captures", "Capture Loss"],
      ["lost a capture at an unknown time", "Capture Loss (Time Unknown)"],
      ["captured an edge without a valid clock", "Invalid Capture Clock"],
      ["reported capture times out of order", "Capture Times Out of Order"],
      ["sent an event the console quarantined", "Invalid Event Quarantined"],
      ["rebooted during the run", "Reboot During Run"],
    ];
    for (const [legacy, label] of cases) {
      assert.equal(formatFaultReason(`Sensor 0250FEB2 ${legacy}.`), `Sensor 0250FEB2: ${label}`);
      assert.equal(formatFaultReason(`Sensor 0250FEB2: ${label}`), `Sensor 0250FEB2: ${label}`);
    }
    const unknown = "Sensor A reported a future fault: code 17.";
    assert.equal(formatFaultReason(unknown), unknown);
    assert.equal(formatFaultReason("The master rebooted during the run."), "The master rebooted during the run.");
  });
  it("preserves the distinction between unknown, infinite and bounded fault intervals", () => {
    assert.equal(faultWindow({ lo: "16000", hi: null }, "0"), "after +00:00.001 (end not known yet)");
    assert.equal(faultWindow({ lo: "16000", hi: "inf" }, "0"), "from +00:00.001 on");
    assert.equal(faultWindow({ lo: "16000", hi: "32000" }, "0"), "between +00:00.001 and +00:00.002");
  });
});
