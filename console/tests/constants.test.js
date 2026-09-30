// The console and the firmware share these values; parse the C headers and compare.
import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import * as protocol from "../src/lib/protocol.js";
import { MASTER_TICKS_PER_MS } from "../src/lib/event-timing.js";
import { WIRELESS_STATUS_MAX_AGE_MS } from "../src/lib/constants.js";

const FW = resolve(__dirname, "../../device/firmware/src");

function defines(file) {
  const out = {};
  for (const line of readFileSync(resolve(FW, file), "utf8").split("\n")) {
    const m = /^\s*#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b/.exec(line);
    if (m) out[m[1]] = Number(m[2]);
  }
  return out;
}

describe("firmware <-> console constants", () => {
  const p = defines("protocol.h");
  const u = defines("proto_usb.h");
  const c = defines("config.h");

  it("protocol.h", () => {
    assert.equal(p.PROTO_VER, protocol.RADIO_PROTO_VERSION);
    assert.equal(p.HEALTH_SYNC_VALID, protocol.FLAG_SYNC);
    assert.equal(p.HEALTH_SKEW_VALID, protocol.FLAG_SKEW);
    assert.equal(p.HEALTH_CLOCK_XTAL, protocol.FLAG_XTAL);
    assert.equal(p.EVENT_INTERPOLATED, protocol.FLAG_INTERPOLATED);
    assert.equal(p.EVENT_TIME_UNKNOWN, protocol.FLAG_TIME_UNKNOWN);
    assert.equal(p.MAX_NODES, protocol.MAX_SENSORS);
    assert.equal(p.UL_EDGES_MAX, protocol.EDGES_PER_LINE_MAX);
  });

  it("proto_usb.h", () => {
    assert.equal(u.PU_USB_PROTO, protocol.USB_PROTO_VERSION);
    for (const [name, bit] of Object.entries(protocol.RESET_BITS)) assert.equal(u[`RESET_${name}`], bit, `RESET_${name}`);
    for (const [name, bit] of Object.entries(protocol.ERROR_BITS)) assert.equal(u[`ERR_${name}`], bit, `ERR_${name}`);
  });

  it("config.h", () => {
    assert.equal(c.TICKS_PER_MS, Number(MASTER_TICKS_PER_MS));
    assert.equal(c.MASTER_EVENT_QUEUE_LEN, protocol.MASTER_QUEUE_CAPACITY);
    assert.equal(c.LINK_OK_MS, WIRELESS_STATUS_MAX_AGE_MS);
    assert.equal(c.LINK_STALE_MS, protocol.LINK_STALE_MS);
  });
});
