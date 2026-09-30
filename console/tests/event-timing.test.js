import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { masterTick, nominalNs } from "../src/lib/event-timing.js";
import { formatDuration } from "../src/lib/format.js";

describe("master ticks", () => {
  it("parses 64-bit ticks exactly and rejects anything else", () => {
    assert.equal(masterTick("18446744073709551615"), (1n << 64n) - 1n);
    assert.equal(masterTick(16000), 16000n);
    assert.throws(() => masterTick("18446744073709551616"), /invalid master tick/);
    assert.throws(() => masterTick("-1"), /invalid master tick/);
    assert.throws(() => masterTick(1.5), /invalid master tick/);
  });
  it("converts a span at the nominal rate, half up", () => {
    assert.equal(nominalNs(16000n), 1_000_000n);
    assert.equal(nominalNs(1n), 63n); // 62.5 ns
    assert.equal(nominalNs(-1n), -63n);
  });
});

describe("formatDuration", () => {
  it("rounds ns to ms half up, once", () => {
    assert.equal(formatDuration(62_531_000_000n), "01:02.531");
    assert.equal(formatDuration(1_000_499_999n), "00:01.000");
    assert.equal(formatDuration(1_000_500_000n), "00:01.001"); // x.5 ms rounds up
    assert.equal(formatDuration(0n), "00:00.000");
    assert.equal(formatDuration(-5n), "00:00.000");
    assert.equal(formatDuration(3_600_000_000_000n), "60:00.000");
    assert.equal(formatDuration(null), "—");
  });
  it("divides before rounding (averages)", () => {
    assert.equal(formatDuration(3_000_000_001n, 2n), "00:01.500");
    assert.equal(formatDuration(2_000_999_999n + 1_000_000_000n, 3n), "00:01.000"); // 1000.333 ms
    assert.equal(formatDuration(1_001n * 1_000_000n + 1n, 2n), "00:00.501"); // 500.5000005 ms
  });
});
