// Camera line timing in Settings, as the camera Pi computes it (config.exposure_max_us,
// protocol.period_for): the console follows a line rate before it is applied.
import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { exposureMaxUs, linePeriod, lineRateAt } from "../src/lib/camera.js";

// /api/config lineTiming
const T = { masterHz: 16_000_000, periodMin: 270, periodMax: 16000, cameraHz: 27_000_000, readoutTicks: 85 };

describe("camera line timing", () => {
  it("runs a line rate at a whole master period, a half rounding to even as on the Pi", () => {
    assert.equal(linePeriod(58000, T), 276);
    assert.equal(linePeriod(59000, T), 271);
    assert.equal(linePeriod(59259, T), 270);
    assert.equal(linePeriod(60000, T), 270);
    assert.equal(linePeriod(51200, T), 312); // 312.5
    assert.equal(linePeriod(10240, T), 1562); // 1562.5
    assert.equal(Math.round(lineRateAt(59000, T)), 59041);
  });

  it("leaves the camera its readout within the period", () => {
    assert.equal(exposureMaxUs(58000, T), 380 / 27);
    assert.equal(exposureMaxUs(59000, T), 372 / 27);
    assert.equal(exposureMaxUs(40000, T), 590 / 27);
  });
});
