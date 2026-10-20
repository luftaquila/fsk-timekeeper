import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { faultWindow } from "../src/lib/fault-text";

describe("saved fault presentation", () => {
  it("preserves the distinction between unknown, infinite and bounded fault intervals", () => {
    assert.equal(faultWindow({ lo: "16000", hi: null }, "0"), "after +00:00.001 (end not known yet)");
    assert.equal(faultWindow({ lo: "16000", hi: "inf" }, "0"), "from +00:00.001 on");
    assert.equal(faultWindow({ lo: "16000", hi: "32000" }, "0"), "between +00:00.001 and +00:00.002");
  });
});
