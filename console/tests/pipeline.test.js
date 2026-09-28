// Store-level integration: fake master -> device.handleLine -> IndexedDB event log ->
// `C` ack -> engine -> history. Runs in node with fake-indexeddb.
import "fake-indexeddb/auto";
import { describe, it, beforeEach, vi } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";

const toasts = { success: [], error: [], warning: [] };
vi.mock("../src/composables/useNotification", () => ({
  useNotification: () => ({
    success: (m) => toasts.success.push(m),
    error: (m) => toasts.error.push(m),
    open: ({ message }) => toasts.warning.push(message),
  }),
}));

globalThis.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 16);
globalThis.cancelAnimationFrame = (id) => clearTimeout(id);
if (!globalThis.localStorage) {
  const mem = new Map();
  globalThis.localStorage = {
    getItem: (k) => (mem.has(k) ? mem.get(k) : null),
    setItem: (k, v) => mem.set(k, String(v)),
    removeItem: (k) => mem.delete(k),
    clear: () => mem.clear(),
  };
}

const eventLog = await import("../src/lib/eventLog.js");
const { useDeviceStore } = await import("../src/stores/device.js");
const { useSettingsStore } = await import("../src/stores/settings.js");
const { useTimingStore } = await import("../src/stores/timing.js");
const { useHistoryStore } = await import("../src/stores/history.js");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, timeoutMs = 4000, step = 20) {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("waitFor timeout");
    await sleep(step);
  }
}

async function boot({ keepStorage = false } = {}) {
  setActivePinia(createPinia());
  if (!keepStorage) localStorage.clear();
  eventLog._reset();
  const timing = useTimingStore();
  const minSeq = timing.restore();
  await eventLog.init({ minSeq });
  eventLog.protect(minSeq);
  const history = useHistoryStore();
  await history.init();
  timing.reevaluate();
  return { device: useDeviceStore(), settings: useSettingsStore(), timing, history };
}

// Connect the simulator and wait until its sensors reported and their first checkpoints are acked.
async function connectFake(device) {
  assert.equal(await device.connect({ kind: "fake" }), true);
  const fake = device.fakeTransport();
  await waitFor(() => device.identity && Object.keys(device.telemetry).length >= 3);
  await waitFor(() => fake.queueLength === 0 && device.stats.acks >= 2);
  return fake;
}

describe("pipeline with the fake master", () => {
  beforeEach(async () => {
    toasts.success.length = toasts.error.length = toasts.warning.length = 0;
    await new Promise((resolve) => {
      const req = indexedDB.deleteDatabase(eventLog.DB_NAME);
      req.onsuccess = req.onerror = req.onblocked = () => resolve();
    });
  });

  it("acks every event, times a sprint run and records it in history", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device);
    assert.equal(device.identityOk, true);
    assert.equal(device.stats.events, device.stats.acks);
    const [a, b] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setMapping(b, { role: "finish" });

    assert.equal(timing.qualityFor("sprint").ok, true, JSON.stringify(timing.qualityFor("sprint").reasons));
    assert.equal(await timing.start("sprint", "car 7"), true, toasts.error.join(" | "));
    assert.equal(timing.run.armed, true);
    assert.equal(timing.lightColor, "green");
    assert.equal(await timing.start("sprint", "again"), false); // one run at a time
    assert.match(toasts.error.at(-1), /already running/);
    assert.equal(history.rows.length, 1);
    assert.equal(history.rows[0].verification, "pending");

    const startTick = fake.crossing(a);
    fake.crossing(b, { offsetMs: 1000 });
    await waitFor(() => timing.live.crossings.length === 2);
    assert.equal(timing.live.crossings[0].role, "start");
    assert.equal(timing.live.crossings[1].role, "finish");
    assert.equal(timing.run.verification, "pending");

    // Both sensors must checkpoint past the finish time before the interval is official.
    await sleep(1100);
    fake.checkpoint(a);
    fake.checkpoint(b);
    await waitFor(() => timing.run.verification === "verified");
    const result = timing.run.result;
    assert.ok(result >= 1000 && result <= 1100, `result ${result}`);
    assert.equal(timing.run.closed, true);
    assert.equal(timing.lightColor, "red");
    assert.ok(timing.live.crossings.every((c) => c.confirmed));
    await waitFor(() => history.rows[0].verification === "verified");
    assert.equal(history.rows[0].result, result);
    assert.equal(history.rows[0].note, "car 7");
    assert.equal(history.rows[0].mode, "sprint");

    await waitFor(() => fake.queueLength === 0);
    assert.equal(device.stats.acks, device.stats.events);
    assert.equal(device.dropped.count, 0);
    assert.ok(eventLog.isDurable());

    // reload: results and the closed run survive
    await device.disconnect();
    const again = await boot({ keepStorage: true });
    assert.equal(again.history.rows.length, 1);
    assert.equal(again.history.rows[0].result, result);
    assert.equal(again.timing.run.result, result);
    assert.equal(again.timing.run.closed, true);
    assert.equal(again.settings.state.lastNote, "car 7");
  });

  it("laps with a target auto-stops and ignores finish-mapped sensors", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device);
    const [a, b] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setMapping(b, { role: "finish" });
    settings.setLapTarget(4);
    assert.deepEqual(timing.qualityFor("laps").mappings.map((m) => m.node_id), [a]);

    assert.equal(await timing.start("laps", "lp"), true, toasts.error.join(" | "));
    assert.equal(timing.run.lapTarget, 4);
    assert.equal(Object.keys(timing.run.nodes).join(), a);
    for (let i = 0; i < 6; i++) fake.crossing(a, { offsetMs: i * 5000 });
    fake.loss(b, 1); // a finish sensor losing captures is irrelevant in laps mode
    fake.checkpoint(a);
    fake.checkpoint(b);
    await waitFor(() => timing.run.verification === "verified" && timing.run.closed, 6000);
    assert.equal(timing.run.lapTicks.length, 4);
    assert.ok(Math.abs(timing.run.result - 20000) <= 5, `laps ${timing.run.result}`);
    assert.equal(timing.run.fault, null);
    assert.equal(timing.lightColor, "red");
    assert.equal(timing.rawLaps.length, 5); // raw list still shows every crossing
    await waitFor(() => history.rows[0].verification === "verified");
    assert.equal(history.rows[0].lapTarget, 4);
    assert.equal(history.rows[0].laps.length, 4);
    await device.disconnect();
  });

  it("laps without a target accumulates until Stop", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setLapTarget("");
    assert.equal(await timing.start("laps", "en"), true, toasts.error.join(" | "));
    assert.equal(timing.run.lapTarget, null);
    for (let i = 0; i < 4; i++) fake.crossing(a, { offsetMs: i * 15000 });
    fake.checkpoint(a);
    await waitFor(() => timing.run.result != null, 6000);
    assert.equal(timing.run.closed, false);
    assert.equal(timing.run.armed, true);
    assert.ok(Math.abs(timing.run.result - 45000) <= 5, `laps ${timing.run.result}`);
    assert.equal(timing.rawLaps.length, 3);
    timing.stop();
    assert.equal(timing.run.closed, true);
    assert.equal(timing.lightColor, "red");
    timing.reset();
    assert.equal(timing.run, null);
    assert.equal(timing.lightColor, "grey");
    await device.disconnect();
  });

  it("a capture loss during a sprint invalidates it and raises a fault", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a, b] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setMapping(b, { role: "finish" });
    assert.equal(await timing.start("sprint"), true, toasts.error.join(" | "));
    fake.crossing(a);
    fake.loss(b, 1);
    fake.checkpoint(a);
    fake.checkpoint(b);
    await waitFor(() => timing.run.verification === "invalid");
    assert.equal(timing.run.armed, false);
    assert.equal(timing.fault.reasons[0].node_id, b);
    assert.ok(toasts.error.some((m) => /lost a capture/.test(m)));
    await device.disconnect();
  });

  it("refuses to start without a master or with missing roles", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    assert.equal(await timing.start("sprint"), false);
    assert.match(toasts.error.at(-1), /not connected/);
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("sprint"), false);
    assert.match(toasts.error.at(-1), /No finish sensor is mapped/);
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    timing.stop();
    await device.disconnect();
  });
  it("freezes the GPS calibration at START and stamps the run with UTC", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device);
    const [a, b] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setMapping(b, { role: "finish" });
    fake.setGps({ ppb: 500000, valid: true, fix: 1, sats: 9, span: 64 }); // +500 ppm: an exact 2 s interval reads 1999 ms
    await waitFor(() => device.pps?.valid === 1 && device.pps.ppb === 500000);
    assert.equal(await timing.start("sprint", "gps"), true, toasts.error.join(" | "));
    assert.equal(timing.run.calib.ppb, 500000);
    assert.match(timing.run.startedUtc, /^\d{4}-\d{2}-\d{2}T/);
    fake.setGps({ ppb: 0 }); // a later change must not affect the frozen run
    const startTick = fake.crossing(a);
    fake.crossing(b, { at: BigInt(startTick) + 2000n * 16000n });
    await sleep(2100); // both sensors checkpoint past the finish
    fake.checkpoint(a);
    fake.checkpoint(b);
    await waitFor(() => timing.run.verification === "verified");
    assert.equal(timing.run.result, 1999);
    await waitFor(() => history.rows[0].verification === "verified");
    assert.equal(history.rows[0].ppb, 500000);
    assert.equal(history.rows[0].startedUtc, timing.run.startedUtc);
    assert.equal(device.dropped.count, 0);
    await device.disconnect();

    // no valid PPS -> nominal
    const again = await boot();
    const fake2 = await connectFake(again.device);
    const [c, d] = [...fake2.sensors.keys()];
    again.settings.setMapping(c, { role: "start" });
    again.settings.setMapping(d, { role: "finish" });
    fake2.setGps({ valid: false, fix: 0 });
    await waitFor(() => again.device.pps && again.device.pps.valid === 0);
    assert.equal(await again.timing.start("sprint"), true, toasts.error.join(" | "));
    assert.equal(again.timing.run.calib, null);
    assert.equal(again.timing.run.startedUtc, null);
    await again.device.disconnect();
  });

});
