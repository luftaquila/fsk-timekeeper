import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { formatLapMs, masterTickDeltaMs, masterTickDurationsMs, masterTickDistanceBelowMs, tickDurationMs } from "../src/lib/event-timing.js";

describe("master-tick arithmetic", () => {
  it("formats ms as MM:SS.mmm", () => {
    assert.equal(formatLapMs(62531), "01:02.531");
    assert.equal(formatLapMs(0), "00:00.000");
    assert.equal(formatLapMs(-5), "00:00.000");
  });

  it("subtracts raw 64-bit ticks before rounding once", () => {
    const start = "8160"; // 0.51 ms
    const finish = "16023840"; // 1001.49 ms, delta = 1000.98 ms
    assert.equal(masterTickDeltaMs(finish, start), 1001);
    assert.equal(masterTickDeltaMs("9007199254740993000", "9007199254724977000"), 1001);
    assert.throws(() => masterTickDeltaMs("18446744073709551616", "0"), /invalid master tick/);
  });

  it("sums lap ticks and rounds the total once", () => {
    assert.equal(masterTickDurationsMs([8000n, 8000n]), 1); // 0.5 ms + 0.5 ms -> 1 ms, not 1 + 1
    assert.equal(masterTickDurationsMs([]), 0);
    assert.equal(masterTickDurationsMs(["16000", 16000n, 16000]), 3);
  });

  it("applies the GPS ppb correction with exact rational rounding", () => {
    // 60 s at nominal = 960 000 000 ticks; +100 ppm fast clock -> 59994 ms, -100 ppm -> 60006 ms
    assert.equal(masterTickDeltaMs("960000000", "0", 100000), 59994);
    assert.equal(masterTickDeltaMs("960000000", "0", -100000), 60006);
    assert.equal(masterTickDeltaMs("960000000", "0", 0), 60000);
    assert.equal(masterTickDurationsMs([480000000n, 480000000n], 100000), 59994);
    assert.equal(tickDurationMs(-16000n, 0), -1);
    assert.equal(tickDurationMs("8000", 0), 1); // half up, unchanged at nominal
    assert.equal(tickDurationMs(7999n, 0), 0);
    assert.throws(() => masterTickDeltaMs("16000", "0", 1.5), /invalid ppb/);
    assert.throws(() => masterTickDeltaMs("16000", "0", 2_000_000), /invalid ppb/);
  });

  it("compares debounce windows in raw ticks without endpoint rounding", () => {
    assert.equal(masterTickDistanceBelowMs("31999", "16000", 1), true);
    assert.equal(masterTickDistanceBelowMs("32000", "16000", 1), false);
    assert.equal(masterTickDistanceBelowMs("32000", "16000", -1), false);
  });
});
