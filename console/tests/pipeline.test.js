// Store-level integration: fake master (USB v2) -> device.handleLine -> IndexedDB event log ->
// `C` ack -> engine -> calibration freeze -> history. Runs in node with fake-indexeddb.
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
const { fakeDefaults } = await import("../src/transport/fake.js");
const { useDeviceStore } = await import("../src/stores/device.js");
const { useSettingsStore } = await import("../src/stores/settings.js");
const { useTimingStore } = await import("../src/stores/timing.js");
const { useHistoryStore } = await import("../src/stores/history.js");
const { resultNs } = await import("../src/lib/results.js");
const { encodeRun } = await import("../src/lib/run-codec.js");

fakeDefaults.slotDelayMin = 10;
fakeDefaults.slotDelayMax = 30;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, timeoutMs = 5000, step = 20) {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("waitFor timeout");
    await sleep(step);
  }
}
const ms = (ns) => Number((BigInt(ns) + 500_000n) / 1_000_000n);

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
  const device = useDeviceStore();
  await timing.reevaluate();
  return { device, settings: useSettingsStore(), timing, history };
}

// Connect the simulator and wait until its sensors reported and their first checkpoints are acked.
async function connectFake(device, { periodicCheckpoints = true } = {}) {
  assert.equal(await device.connect({ kind: "fake" }), true);
  const fake = device.fakeTransport();
  fake.setPeriodicCheckpoints(periodicCheckpoints);
  await waitFor(() => device.contract.ok && Object.keys(device.telemetry).length >= 3);
  await waitFor(() => fake.queueLength === 0 && device.stats.acks >= 2);
  return fake;
}

async function mapSprint(device, settings) {
  const fake = await connectFake(device);
  const [a, b] = [...fake.sensors.keys()];
  settings.setMapping(a, { role: "start" });
  settings.setMapping(b, { role: "finish" });
  return { fake, a, b };
}

describe("pipeline with the fake master", () => {
  beforeEach(async () => {
    toasts.success.length = toasts.error.length = toasts.warning.length = 0;
    await new Promise((resolve) => {
      const req = indexedDB.deleteDatabase(eventLog.DB_NAME);
      req.onsuccess = req.onerror = req.onblocked = () => resolve();
    });
  });

  it("acks every event, decides a sprint on its two first crossings, freezes the calibration and records it", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    assert.equal(device.identityOk, true);
    await waitFor(() => device.ppsEdges.length >= 2, 4000); // S1 inside PPS coverage
    assert.equal(timing.qualityFor("sprint").ok, true, JSON.stringify(timing.qualityFor("sprint").reasons));
    assert.equal(await timing.start("sprint", "car 7"), true, toasts.error.join(" | "));
    assert.equal(timing.lightColor, "green");
    assert.equal(await timing.start("sprint", "again"), false);
    assert.match(toasts.error.at(-1), /already running/);
    assert.equal(history.rows[0].verification, "pending");

    fake.crossing(a);
    fake.crossing(b, { offsetMs: 1000 });
    await waitFor(() => timing.run.verification === "verified");
    assert.equal(timing.run.closed, true);
    assert.equal(timing.lightColor, "red");
    assert.ok(timing.live.crossings.every((c) => c.confirmed));
    await waitFor(() => timing.run.durationNs != null, 4000);
    assert.equal(timing.run.calibration.method, "gps");
    const result = ms(timing.run.durationNs);
    assert.ok(result >= 1000 && result <= 1002, `result ${result}`);
    await waitFor(() => history.rows[0].durationNs != null);
    assert.equal(history.rows[0].verification, "verified");
    assert.equal(history.rows[0].durationNs, timing.run.durationNs);
    assert.equal(history.rows[0].note, "car 7");
    const csv = history.csvText();
    assert.match(csv, new RegExp(`,${timing.run.durationNs},${timing.run.startTick},${timing.run.finishTick},`));

    await waitFor(() => fake.queueLength === 0);
    assert.equal(device.stats.acks + device.stats.duplicates >= device.stats.events - device.dropped.count, true);
    assert.equal(device.dropped.reasons["event.crc"] ?? 0, 0);
    assert.ok(eventLog.isDurable());

    // reload: results and the closed run survive
    await device.disconnect();
    const again = await boot({ keepStorage: true });
    assert.equal(again.history.rows.length, 1);
    assert.equal(again.history.rows[0].durationNs, timing.run.durationNs);
    assert.equal(again.timing.run.closed, true);
    assert.equal(again.timing.resultNs, BigInt(timing.run.durationNs));
    assert.equal(again.settings.state.lastNote, "car 7");
  });

  it("ignores a finish before the first start (PLAN case F) and bundles a burst into one line", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    fake.setSlotDelay(200, 200);
    assert.equal(await timing.start("sprint"), true, toasts.error.join(" | "));
    const linesBefore = device.stats.events;
    const t0 = BigInt(fake.nowTick());
    fake.crossing(b, { at: t0 + 500n * 16000n });
    fake.crossing(a, { at: t0 + 1000n * 16000n });
    for (const offset of [1050n, 1100n, 1150n]) fake.crossing(a, { at: t0 + offset * 16000n }); // bounces, same bundle
    fake.crossing(b, { at: t0 + 5000n * 16000n });
    await waitFor(() => timing.run.verification === "verified");
    assert.equal(timing.run.fault, null);
    assert.equal(BigInt(timing.run.finishTick) - BigInt(timing.run.startTick), 4000n * 16000n);
    assert.ok(device.stats.events - linesBefore <= 3, `lines ${device.stats.events - linesBefore}`); // a's four edges came as one C line
    const rows = eventLog.since(timing.run.cursor).filter((r) => r.node_id === a && r.kind === "capture");
    assert.equal(rows.length, 4);
    assert.equal(new Set(rows.map((r) => r.hseq)).size, 1);
    await device.disconnect();
  });

  it("laps with a target auto-stops and ignores finish-mapped sensors", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    settings.setLapTarget(4);
    assert.deepEqual(timing.qualityFor("laps").mappings.map((m) => m.node_id), [a]);
    assert.equal(await timing.start("laps", "lp"), true, toasts.error.join(" | "));
    assert.equal(Object.keys(timing.run.nodes).join(), a);
    const t0 = BigInt(fake.nowTick());
    for (let i = 0; i < 6; i++) fake.crossing(a, { at: t0 + BigInt(i * 5000) * 16000n });
    fake.loss(b, 1); // a finish sensor losing captures is irrelevant in laps mode
    await waitFor(() => timing.run.closed, 6000);
    assert.equal(timing.run.verification, "verified");
    assert.equal(timing.run.crossingTicks.length, 5);
    await waitFor(() => timing.run.durationNs != null, 4000);
    assert.equal(ms(timing.run.durationNs), 20000);
    assert.equal(timing.lapRows.length, 4);
    assert.ok(timing.lapRows.every((l) => ms(l.ns) === 5000));
    await waitFor(() => history.rows[0].durationNs != null);
    assert.equal(history.rows[0].lapTarget, 4);
    assert.equal(history.rows[0].crossingTicks.length, 5);
    await device.disconnect();
  });

  it("freezes the debounce at START — a later setting change or a restart never regroups crossings", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setDebounceMs(300);
    assert.equal(await timing.start("laps", "db"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    for (const offset of [200, 1000, 1200]) fake.crossing(a, { at: t0 + BigInt(offset) * 16000n }); // 200 / 1200 bounce inside 300 ms
    await waitFor(() => timing.run.crossingTicks.length === 2);
    assert.deepEqual(timing.lapRows.map((l) => ms(l.ns)), [1000]);
    settings.setDebounceMs(2000); // next START only
    fake.crossing(a, { at: t0 + 3000n * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 3);
    assert.deepEqual(timing.lapRows.map((l) => ms(l.ns)), [1000, 2000]);
    assert.equal(timing.run.debounceMs, 300);
    await device.disconnect();

    const again = await boot({ keepStorage: true });
    assert.equal(again.settings.state.debounceMs, 2000);
    assert.equal(again.timing.run.debounceMs, 300);
    assert.deepEqual(again.timing.lapRows.map((l) => ms(l.ns)), [1000, 2000]);
  });

  it("laps without a target runs until Stop, then closes once the sensor confirms through the stop tick", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device, { periodicCheckpoints: false });
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setLapTarget("");
    assert.equal(await timing.start("laps", "en"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    for (const offset of [400, 800, 1200]) fake.crossing(a, { at: t0 + BigInt(offset) * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 4);
    assert.equal(timing.run.verification, "verified"); // so far
    assert.equal(timing.run.closed, false);
    assert.equal(ms(timing.resultNs), 1200);
    await sleep(1300);
    const cpBefore = fake.cpRequests;
    assert.equal(await timing.stop(), true);
    assert.equal(timing.run.armed, false);
    assert.equal(timing.run.verification, "pending");
    await waitFor(() => fake.cpRequests > cpBefore);
    await waitFor(() => timing.run.closed, 6000);
    assert.equal(timing.run.verification, "verified");
    await waitFor(() => timing.run.durationNs != null, 4000);
    assert.equal(ms(timing.run.durationNs), 1200);
    timing.reset();
    assert.equal(timing.run, null);
    assert.equal(timing.lightColor, "grey");
    await device.disconnect();
  });

  it("a finish-sensor loss between S1 and F1 invalidates the sprint", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    assert.equal(await timing.start("sprint"), true, toasts.error.join(" | "));
    fake.crossing(a);
    await sleep(50);
    fake.loss(b, 1);
    await waitFor(() => timing.run.verification === "invalid");
    assert.equal(timing.run.closed, true);
    assert.equal(timing.fault.reasons[0].node_id, b);
    assert.ok(toasts.error.some((m) => /lost captures/.test(m)));
    await device.disconnect();
  });

  it("Stop without a finish is DNF; without a start DNS", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a } = await mapSprint(device, settings);
    assert.equal(await timing.start("sprint", "dnf"), true, toasts.error.join(" | "));
    fake.crossing(a);
    await waitFor(() => timing.live.crossings.length === 1);
    await sleep(100);
    assert.equal(await timing.stop(), true);
    await waitFor(() => timing.run.closed, 6000);
    assert.equal(timing.run.verification, "dnf");
    assert.equal(timing.run.dnfReason, "DNF");
    await waitFor(() => history.rows[0].verification === "dnf");
    assert.equal(history.status(history.rows[0]), "DNF");
    timing.reset();

    assert.equal(await timing.start("sprint", "dns"), true, toasts.error.join(" | "));
    await sleep(100);
    assert.equal(await timing.stop(), true);
    await waitFor(() => timing.run.closed, 6000);
    assert.equal(timing.run.dnfReason, "DNS");
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
    await timing.stop();
    await device.disconnect();
  });

  it("Stop needs the master's own clock answer: refused when `T` fails or the master is gone", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps", "clock"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    fake.crossing(a, { at: t0 + 400n * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 2);
    const realReadClock = device.readClock;
    device.readClock = () => Promise.reject(new Error("no reply"));
    assert.equal(await timing.stop(), false);
    assert.match(toasts.error.at(-1), /did not answer/);
    assert.equal(timing.run.armed, true);
    assert.equal(timing.run.stopTick, null);
    device.readClock = realReadClock;
    await sleep(450);
    assert.equal(await timing.stop(), true, toasts.error.join(" | "));
    assert.ok(BigInt(timing.run.stopTick) > t0 + 400n * 16000n);
    timing.reset();

    assert.equal(await timing.start("laps", "offline"), true, toasts.error.join(" | "));
    fake.crossing(a);
    await waitFor(() => timing.live.crossings.length === 1);
    await device.disconnect();
    assert.equal(await timing.stop(), false);
    assert.match(toasts.error.at(-1), /not connected/);
    assert.equal(timing.run.armed, true);
  });

  it("converts through the PPS timeline (+500 ppm → 1999 ms) and stamps the run with UTC", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    fake.setGps({ ppb: 500000 });
    await waitFor(() => device.ppsEdges.length >= 3, 6000);
    assert.equal(await timing.start("sprint", "gps"), true, toasts.error.join(" | "));
    assert.match(timing.run.startedUtc, /^\d{4}-\d{2}-\d{2}T/);
    assert.equal(timing.run.gpsAtStart.ppb, 500000);
    const s0 = BigInt(fake.crossing(a));
    fake.crossing(b, { at: s0 + 2000n * 16000n });
    await waitFor(() => timing.run.durationNs != null, 8000);
    assert.equal(ms(timing.run.durationNs), 1999);
    assert.ok(["gps", "gps-extrapolated"].includes(timing.run.calibration.method));
    await waitFor(() => history.rows[0].durationNs != null);
    assert.equal(ms(resultNs(history.rows[0])), 1999);
    await device.disconnect();

    // no qualified PPS -> nominal
    const again = await boot();
    const fake2 = await connectFake(again.device);
    const [c, d] = [...fake2.sensors.keys()];
    again.settings.setMapping(c, { role: "start" });
    again.settings.setMapping(d, { role: "finish" });
    fake2.setGps({ valid: false, fix: 0 });
    fake2.rebootMaster(); // a fresh boot has no qualified edge yet
    await waitFor(() => again.device.contract.ok && again.device.masterBootId === fake2.masterBootId);
    await waitFor(() => fake2.queueLength === 0 && Object.values(again.device.telemetry).every((t) => t.node_id === "0" || t.master_boot_id === fake2.masterBootId), 8000);
    await sleep(5200); // periodic checkpoints under the new session
    assert.equal(await again.timing.start("sprint"), true, toasts.error.join(" | "));
    const c0 = BigInt(fake2.crossing(c));
    fake2.crossing(d, { at: c0 + 1500n * 16000n });
    await waitFor(() => again.timing.run.durationNs != null, 6000);
    assert.equal(again.timing.run.calibration.method, "nominal");
    assert.equal(ms(again.timing.run.durationNs), 1500);
    assert.equal(again.timing.run.startedUtc, null);
    await again.device.disconnect();
  });

  it("a master reboot ends an open laps run: laps before stay confirmed, no total", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    fake.crossing(a, { at: t0 + 1000n * 16000n });
    fake.crossing(a, { at: t0 + 2000n * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 3);
    fake.rebootMaster();
    await waitFor(() => timing.run.verification === "invalid");
    assert.equal(timing.run.closed, true);
    assert.equal(timing.run.totalValid, false);
    assert.equal(timing.resultNs, null);
    assert.equal(timing.run.crossingTicks.length, 3);
    await waitFor(() => timing.lapRows.length === 2 && timing.lapRows.every((l) => l.ns != null), 4000);
    assert.deepEqual(timing.lapRows.map((l) => ms(l.ns)), [1000, 1000]);
    await device.disconnect();
  });

  it("quarantines a valid-crc but invalid line, acks it, and treats the gap as a hole", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    await waitFor(() => timing.run.crossingTicks.length === 1);
    fake.injectBadLine(a);
    await waitFor(() => device.stats.quarantined === 1);
    await waitFor(() => fake.queueLength === 0); // acked
    assert.ok(device.quarantine);
    assert.ok(device.logEntries.some((e) => e.code === "quarantine"));
    const listed = await eventLog.listQuarantine();
    assert.equal(listed.length, 1);
    fake.crossing(a, { at: t0 + 2000n * 16000n }); // beyond the debounce window of the first crossing
    await waitFor(() => timing.run.verification === "invalid");
    assert.match(timing.fault.reasons[0].reason, /quarantined/);
    await device.disconnect();
  });

  it("never acks a line with a bad crc; the re-sent copy is taken", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    const acks = device.stats.acks;
    fake.corruptNext();
    fake.crossing(a);
    await waitFor(() => timing.run.crossingTicks.length === 1);
    assert.equal(device.dropped.reasons["event.crc"], 1);
    assert.equal(device.stats.acks, acks + 1);
    await device.disconnect();
  });

  it("refuses START while the master keeps re-sending the same event", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    fake.setCorrupt(true);
    fake.crossing(a);
    await waitFor(() => device.head != null && Date.now() - device.head.firstSeenAt > 5200, 8000);
    assert.equal(timing.qualityFor("laps").ok, false);
    assert.match(timing.qualityFor("laps").reasons[0].reason, /keeps re-sending/);
    assert.equal(await timing.start("laps"), false);
    fake.setCorrupt(false);
    await waitFor(() => fake.queueLength === 0);
    await waitFor(() => timing.qualityFor("laps").ok, 3000);
    await device.disconnect();
  });

  it("gates E/D/T/P on the contract: a sensor-role board is never acked and cannot START", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    fake.setRole("S");
    await waitFor(() => !device.contract.ok);
    assert.match(device.contract.reason, /booted as a sensor/);
    const acks = device.stats.acks;
    fake.crossing(a);
    await sleep(400);
    assert.equal(device.stats.acks, acks);
    assert.ok(fake.queueLength >= 1);
    assert.equal(await timing.start("laps"), false);
    assert.match(toasts.error.at(-1), /booted as a sensor/);
    fake.setRole("M");
    await waitFor(() => fake.queueLength === 0);
    assert.ok(device.stats.acks > acks);
    fake.setProto({ usb: 1 });
    await waitFor(() => !device.contract.ok);
    assert.match(device.contract.reason, /versions do not match/);
    await device.disconnect();
  });

  it("logs X lines and boot reasons, warns on ver_drop, and reports storage failure", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device);
    assert.ok(device.logEntries.some((e) => e.kind === "boot" && e.code === "power-on"));
    fake.emitError("radio_reset", 3);
    await waitFor(() => device.logEntries.some((e) => e.kind === "X" && e.code === "radio_reset"));
    fake.setMasterDiag({ verDrop: 1 });
    fake.setMasterDiag({ verDrop: 2 });
    await waitFor(() => device.verDrop?.count === 2);

    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    eventLog.reportWriteFailure(new Error("disk full"));
    assert.equal(device.durable, false);
    assert.ok(device.alarms.some((x) => x.key === "storage"));
    assert.equal(timing.run.durable, false);
    await waitFor(() => history.rows[0].durable === false);
    fake.crossing(a);
    await waitFor(() => timing.run.crossingTicks.length === 1); // acks keep flowing from memory
    await device.disconnect();
  });

  it("closes a stored run of another protocol version and keeps its history row", { timeout: 20000 }, async () => {
    const first = await boot();
    const fake = await connectFake(first.device);
    const [a] = [...fake.sensors.keys()];
    first.settings.setMapping(a, { role: "start" });
    assert.equal(await first.timing.start("laps", "old"), true, toasts.error.join(" | "));
    await first.device.disconnect();
    const stored = { ...first.timing.run, schema: 2, usbProto: 1 };
    localStorage.setItem("tk.run.v3", encodeRun(stored));
    const again = await boot({ keepStorage: true });
    assert.equal(again.timing.run.closed, true);
    assert.equal(again.timing.run.verification, "invalid");
    assert.match(again.timing.run.fault.reasons[0].reason, /protocol change/);
    assert.equal(again.history.rows[0].verification, "invalid");
  });
});
