// Store-level integration: fake master (USB v7) -> device.handleLine -> IndexedDB event log ->
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

const { crc32Hex } = await import("../src/lib/crc32.js");
const deviceLog = await import("../src/lib/deviceLog.js");
const eventLog = await import("../src/lib/eventLog.js");
const { fakeDefaults } = await import("../src/transport/fake.js");
const { useDeviceStore } = await import("../src/stores/device.js");
const { useSettingsStore } = await import("../src/stores/settings.js");
const { useTimingStore } = await import("../src/stores/timing.js");
const { useHistoryStore } = await import("../src/stores/history.js");
const { resultNs } = await import("../src/lib/results.js");
const { formatDuration } = await import("../src/lib/format.js");
const { timingHealth } = await import("../src/lib/quality.js");
const runLog = await import("../src/lib/runLog.js");
const { getAll } = await import("../src/lib/idb.js");

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
  await runLog.flush();
  setActivePinia(createPinia());
  if (!keepStorage) localStorage.clear();
  eventLog._reset();
  const timing = useTimingStore();
  await eventLog.init();
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
  it("tracks initial synchronization by sensor and master boot across reconnects", async () => {
    const { device } = await boot();
    const identity = "I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1";
    const report = ({ skew = 0, valid = 1, sensorBoot = 8002, masterBoot = 8001 } = {}) =>
      `D 0250FEB2 OK 0 0 0 0 -60.00 8.00 0 300 4000 0 1 1 ${skew} XTAL 100 0 0 0 0 0 0 0 0 ${sensorBoot} ${masterBoot} 0 0 0 0 ${valid}`;
    const health = () => timingHealth(device.telemetry["0250FEB2"]);
    await device.handleLine(identity);
    await device.handleLine(report({ valid: 0 }));
    assert.equal(health().label, "CONNECTING");
    await device.handleLine(report());
    assert.equal(health().label, "SYNCING");
    assert.equal(health().ok, false);
    await device.handleLine(report({ skew: 1 }));
    assert.equal(health().label, "OK");
    await device.handleLine(report());
    assert.equal(health().label, "SYNC INVALID");

    // Reauthentication cannot turn an established clock's failure into startup.
    await device.handleLine("J 0250FEB2 WAIT");
    await device.handleLine(report({ valid: 0 }));
    await device.handleLine("J 0250FEB2 SYNC");
    await device.handleLine(report());
    assert.equal(health().label, "SYNC INVALID");
    await device.disconnect();
    await device.handleLine(identity);
    await device.handleLine(report());
    assert.equal(health().label, "SYNC INVALID");

    await device.handleLine(report({ sensorBoot: 8003 }));
    assert.equal(health().label, "SYNCING");
    await device.handleLine(report({ sensorBoot: 8003, skew: 1 }));
    assert.equal(health().label, "OK");
    await device.handleLine(report({ sensorBoot: 8003, masterBoot: 8004 }));
    assert.equal(health().label, "SYNCING");
  });

  it("invalidates retained reports across USB sessions until new telemetry arrives", async () => {
    const { device } = await boot();
    const identity = "I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1";
    const report = "D 0250FEB2 OK 0 0 0 0 -60.00 8.00 0 300 4000 0 1 1 1 XTAL 100 0 0 0 0 0 0 0 0 8002 8001 0 0 0 0 1";
    await device.handleLine(identity);
    await device.handleLine(report);
    assert.equal(Number.isFinite(device.telemetry["0250FEB2"].received_at), true);
    await device.disconnect();
    assert.equal(device.telemetry["0250FEB2"].received_at, null);
    assert.equal(device.telemetry["0250FEB2"].link_state, "online");
    await device.handleLine(identity);
    assert.equal(device.telemetry["0250FEB2"].received_at, null);
    await device.handleLine(report);
    assert.equal(Number.isFinite(device.telemetry["0250FEB2"].received_at), true);
  });
  it("keeps removed sensors hidden through cached LOST reports and restores them on reception", async () => {
    const { device, settings } = await boot();
    await device.handleLine("I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1");
    settings.setMapping("0250FEB2", { role: "start", note: "old gate" });
    settings.removeSensor("0250FEB2");
    const report = (state) => `D 0250FEB2 ${state} 0 0 0 0 -60.00 8.00 0 300 4000 0 1 1 1 XTAL 100 0 0 0 0 0 0 0 0 8002 8001 0 0 0 0 1`;
    for (const state of ["SYNC", "WAIT", "READY", "SYNC"]) {
      await device.handleLine(report("LOST"));
      await device.handleLine(`J 0250FEB2 ${state}`);
      assert.deepEqual(settings.state.removedSensors, ["0250FEB2"]);
    }
    assert.deepEqual(settings.state.removedSensors, ["0250FEB2"]);
    await device.handleLine(report("OK"));
    assert.deepEqual(settings.state.removedSensors, []);
    assert.equal(settings.mappingOf("0250FEB2"), null);
    assert.equal(device.telemetry["0250FEB2"].link_state, "online");
  });
  it("keeps the firmware upgrade lock across disconnect and blocks measurement/configuration", async () => {
    const { device, timing } = await boot();
    device.updateInProgress = true;
    assert.equal(await device.connect({ kind: "fake" }), false);
    assert.equal(await timing.start("sprint"), false);
    await assert.rejects(device.provisionKey("a".repeat(64)), /firmware upgrade/);
    await device.disconnect();
    assert.equal(device.updateInProgress, true);
    device.updateInProgress = false;
  });
  it("keeps older log pages available live and after reload beyond 500 entries", async () => {
    const { device } = await boot();
    await device.clearLog();
    for (let i = 1; i <= 510; i++) await device.handleLine(`X queue_full ${i}`);
    await waitFor(() => device.logEntries.length === 510);
    assert.equal(device.logEntries.length, 510);
    assert.equal(device.logEntries[0].text, "X queue_full 510");
    assert.equal(device.logEntries.at(-1).text, "X queue_full 1");
    device.logEntries = [];
    await device.loadLog();
    assert.equal(device.logEntries.length, 510);
    assert.equal(device.logEntries.at(-1).text, "X queue_full 1");
    assert.ok(device.logEntries.every((e) => e.kind === "X"));
    await device.clearLog();
  });

  it("expires PPS by report arrival and on disconnect or a new master boot", async () => {
    const { device } = await boot();
    const identity = "I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1";
    await device.handleLine(identity);
    await device.handleLine("T 00000000000000000000000000000000 0 1234");
    let now = Date.now();
    const dateNow = vi.spyOn(Date, "now").mockImplementation(() => now);
    try {
      await device.handleLine("P 16000000 0 0 0 0 0 0 0 0");
      assert.equal(device.gpsReport().tick, "16000000");
      now += 2999;
      assert.ok(device.gpsReport());
      now++;
      assert.equal(device.gpsReport(), null);
      await device.handleLine("P 64000000 0 50000 0 0 0 8 0 0");
      assert.equal(device.gpsReport().ppb, 50000);
      assert.equal(device.ppsEdges.length, 0);

      await device.disconnect();
      assert.equal(device.gpsReport(), null);
      await device.handleLine(identity);
      await device.handleLine("T 00000000000000000000000000000000 64000000 1234");
      await device.handleLine("P 80000000 0 50000 0 0 0 8 0 0");
      assert.equal(device.gpsReport().tick, "80000000");

      await device.handleLine("T 00000000000000000000000000000000 0 5678");
      assert.equal(device.gpsReport(), null);
      await device.handleLine("P 16000000 0 0 0 0 0 0 0 0");
      assert.equal(device.gpsReport().tick, "16000000"); // a new boot restarts its ticks
      assert.equal(device.gpsReport().span, 0);
    } finally {
      dateNow.mockRestore();
    }
  });

  it("keeps no-fix drift reports out of calibration even when a segment is supplied", async () => {
    const { device } = await boot();
    await device.handleLine("I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1");
    await device.handleLine("T 00000000000000000000000000000000 0 1234");
    for (const valid of [0, 1]) {
      await device.handleLine(`P ${16000800 * (valid + 1)} 0 50000 ${valid} 0 0 8 1 ${valid}`);
      assert.equal(device.gpsReport().ppb, 50000);
      assert.equal(device.gpsReport().span, 8);
      assert.equal(device.ppsEdges.length, 0);
    }
    // Qualification is still required even when GGA reports a fix.
    await device.handleLine("P 48002400 0 50000 0 1 9 8 0 0");
    assert.equal(device.ppsEdges.length, 0);
    await device.handleLine("P 64003200 0 50000 1 1 9 8 2 0");
    assert.equal(device.ppsEdges.length, 1);
    const stored = await (await import("../src/lib/ppsLog.js")).loadEdges(device.masterBootId);
    assert.deepEqual(stored.map((e) => e.tick), ["64003200"]);
  });

  it("manual classification works offline, survives reload and does not recreate a deleted record", async () => {
    let { timing, history } = await boot();
    assert.equal(await timing.classify("DNS", "sprint", "Car 7"), true);
    const id = timing.run.runId;
    assert.equal(history.rows.length, 1);
    assert.equal(history.rows[0].note, "Car 7");
    assert.equal(resultNs(history.rows[0]), null);
    assert.equal(timing.armed, false);
    ({ timing, history } = await boot({ keepStorage: true }));
    assert.equal(timing.run.classification, "DNS");
    assert.equal(history.status(history.rows[0]), "DNS");
    await timing.classify("DNF");
    await timing.classify("DSQ");
    assert.equal(history.rows.length, 1);
    assert.equal(history.rows[0].runId, id);
    assert.equal(history.status(history.rows[0]), "DSQ");
    await timing.classify(null);
    assert.equal(history.rows.length, 0);
    assert.equal(timing.run.historyId, null);
    ({ timing, history } = await boot({ keepStorage: true }));
    assert.equal(timing.run.classification, null);
    assert.equal(history.rows.length, 0);
    assert.equal(await timing.classify("DNS"), true);
    assert.equal(history.rows[0].runId, id);
    assert.equal(history.status(history.rows[0]), "DNS");
    await history.removeRow(history.rows[0].id);
    ({ timing, history } = await boot({ keepStorage: true }));
    assert.equal(history.rows.length, 0);
    timing.reset();
  });

  it("classifying an active attempt closes it and ignores subsequent crossings", async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    assert.equal(await timing.start("sprint"), true);
    fake.crossing(a);
    await waitFor(() => timing.live.crossings.length === 1);
    assert.equal(await timing.classify("DNF"), true);
    const id = timing.run.runId;
    fake.crossing(b, { offsetMs: 800 });
    await sleep(100);
    assert.equal(timing.armed, false);
    assert.equal(timing.run.finishTick, null);
    assert.equal(history.rows.find((r) => r.runId === id).classification, "DNF");
    assert.equal(resultNs(history.rows.find((r) => r.runId === id)), null);
    await timing.classify(null);
    assert.equal(timing.armed, false, "clearing a decision must not resume measurement");
    assert.equal(history.rows.length, 0);
    await device.disconnect();
    timing.reset();
  });

  it("persists checkpoint history even when a transient cleared before telemetry, and deduplicates replay/reload", async () => {
    const { device } = await boot();
    const identity = "I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1";
    await device.handleLine(identity);
    const diag = (flags, boot = "7001") =>
      `D 0250FEB2 OK 0 0 0 0 -60.00 8.00 0 300 4000 0 1 1 1 XTAL 100 0 0 0 0 ${flags} 0 0 0 ${boot} 8001 0 0 0 0 1`;
    const checkpoint = (hseq, evSeq, counters = "0 2 0 0 0 1", recovery = "3 1 3 2", boot = "7001", node = "0250FEB2") => {
      const body = `E ${hseq} ${node} K ${evSeq} 7 8001 ${boot} 100 -60.00 8.00 0 ${1000 + hseq} ${counters} ${recovery}`;
      return `${body} ${crc32Hex(body)}`;
    };
    const history = (store = device) => store.logEntries.filter((e) => e.code === "radio_history");
    // The only live report is already healthy: history must come from the sensor, not D transitions.
    await device.handleLine(diag(0));
    const first = checkpoint(1, 1);
    await Promise.all([device.handleLine(first), device.handleLine(first)]);
    assert.equal(history().length, 1);
    assert.match(history()[0].text, /SPI timeouts: 2 since boot/);
    assert.match(history()[0].text, /reinitializations: 3, failed: 1.*receiver restarted/);
    assert.equal(history()[0].nodeBootId, "7001");
    assert.deepEqual(eventLog.since(0).find((r) => r.hseq === 1).radio_history.faults, [0, 2, 0, 0, 0, 1]);
    assert.equal(device.telemetry["0250FEB2"].err_flags, 0);
    await device.handleLine(checkpoint(2, 2)); // identical cumulative summary, new checkpoint
    assert.equal(history().length, 1);
    await Promise.all([device.handleLine(diag(0x2004)), device.handleLine(diag(0x2004))]);
    assert.equal(device.logEntries.filter((e) => e.code === "radio_fault").length, 1);
    await device.handleLine(diag(0));
    assert.equal(device.logEntries.filter((e) => e.code === "radio_restored").length, 1);
    // Reload from the actual database, not just the in-memory store.
    eventLog._reset();
    await eventLog.init();
    setActivePinia(createPinia());
    const reloaded = useDeviceStore();
    await reloaded.loadLog();
    await reloaded.handleLine(identity);
    await reloaded.handleLine(first);
    await reloaded.handleLine(diag(0));
    assert.equal(history(reloaded).length, 1);
    await reloaded.handleLine(checkpoint(3, 3, "0 65535 0 0 0 1", "65535 1 3 2"));
    assert.match(history(reloaded)[0].text, /65535\+/);
    await reloaded.handleLine(checkpoint(4, 1, "0 1 0 0 0 0", "1 0 2 2", "7002"));
    await reloaded.handleLine(checkpoint(5, 1, "0 1 0 0 0 0", "1 0 2 2", "7001", "0250FEB3"));
    assert.equal(history(reloaded).length, 4); // reboot and another sensor have independent histories
    // Recovery after a crash between event storage and display-log creation.
    await deviceLog.clearEntries();
    setActivePinia(createPinia());
    const rebuilt = useDeviceStore();
    await rebuilt.loadLog();
    assert.equal(history(rebuilt).length, 4);
    await rebuilt.clearLog();
    await rebuilt.loadLog();
    assert.equal(rebuilt.logEntries.length, 0); // intentional clear is not crash recovery
    setActivePinia(createPinia());
    const afterClear = useDeviceStore();
    await afterClear.loadLog();
    assert.equal(afterClear.logEntries.length, 0);
  });

  it("stores checkpoint history before ACK, including a long gap between reports", async () => {
    const { device } = await boot();
    const fake = await connectFake(device);
    const [node] = fake.sensors.keys();
    const sensor = fake.sensors.get(node);
    const write = fake.write;
    let checked = false;
    const spy = vi.spyOn(fake, "write").mockImplementation(async (line) => {
      const checkpoint = eventLog.since(0).find((r) => r.node_id === node && r.radio_history?.faults[1] === 7);
      if (line.startsWith("C ") && checkpoint && Number(line.split(" ")[1]) === checkpoint.hseq) {
        const logs = await deviceLog.listEntries();
        assert.ok(logs.some((e) => e.nodeId === node && e.code === "radio_history" && e.historySnapshot.faults[1] === 7));
        checked = true;
      }
      return write(line);
    });
    try {
      sensor.evSeq += 40000; // more than half the 16-bit sequence space while disconnected
      sensor.faultCounts = [0, 7, 0, 0, 0, 0];
      sensor.recovery = { count: 1, failures: 0, reason: 3, result: 2 };
      fake.checkpoint(node, { immediate: true });
      await waitFor(() => checked);
      assert.ok(device.logEntries.some((e) => e.nodeId === node && /SPI timeouts: 7/.test(e.text)));
    } finally {
      spy.mockRestore();
      await device.disconnect();
    }
  });

  beforeEach(async () => {
    toasts.success.length = toasts.error.length = toasts.warning.length = 0;
    await new Promise((resolve) => {
      const req = indexedDB.deleteDatabase(eventLog.DB_NAME);
      req.onsuccess = req.onerror = req.onblocked = () => resolve();
    });
  });

  it("preserves rejected startup lines after the serial console evicts them", async () => {
    const { device } = await boot();
    const first = "broken startup fragment";
    await device.handleLine(first);
    assert.equal(device.dropped.reasons.unknown_line, 1);
    assert.ok(device.logEntries.some((e) => e.code === "unknown_line" && e.text === first));
    await device.handleLine("I FSK-WL 7 1.0.0 0123456789ABCDEF M 17 922900 7 250.00 16000 0 12 1");
    for (const line of ["D malformed", "P malformed", "E malformed"]) await device.handleLine(line);
    await waitFor(() => ["tel_format", "pps", "event_unreadable"].every((code) => device.logEntries.some((e) => e.code === code)));
    const secret = "ab".repeat(32);
    await device.handleLine(secret); // a bare key fragment must not leak into either log
    for (let i = 0; i < 310; i++) await device.handleLine("A PING OK");
    assert.equal(device.consoleLines.length, 300);
    assert.ok(!device.consoleLines.some((e) => e.text === first));
    await device.loadLog();
    assert.ok(device.logEntries.some((e) => e.code === "unknown_line" && e.text === first));
    assert.ok(!JSON.stringify(await device.exportLog()).includes(secret));
    assert.ok(device.logEntries.some((e) => e.code === "unknown_line" && e.text === "****"));
  });

  it("loads keys per connection, redacts logs, and handles missing/old/invalid replies", { timeout: 10000 }, async () => {
    const { device } = await boot();
    const fake = await connectFake(device);
    const initial = "0123456789abcdef".repeat(4);
    const replacement = "ab".repeat(32);
    try {
      assert.equal(device.radioKey, initial);
      assert.equal(device.keyReadState, "ready");
      assert.equal(await device.provisionKey(replacement), "ok");
      assert.equal(device.radioKey, replacement);
      device.readRadioKey();
      await waitFor(() => device.keyReadState === "ready");
      assert.equal(device.radioKey, replacement);
      assert.ok(device.consoleLines.some((line) => line.dir === "rx" && line.text === "K ****"));
      assert.ok(!JSON.stringify(device.consoleLines).includes(replacement));
      assert.ok(!JSON.stringify(device.logEntries).includes(replacement));
      fake.setProvisioned(false);
      device.readRadioKey();
      await waitFor(() => device.keyReadState === "empty");
      assert.equal(device.radioKey, "");
      const write = fake.write;
      const spy = vi.spyOn(fake, "write");
      for (const [reply, state] of [["X badcmd", "unsupported"], ["K broken", "invalid"]]) {
        spy.mockImplementation(async (line) => {
          if (line === "?KEY") { await device.handleLine(reply); return true; }
          return write(line);
        });
        device.readRadioKey();
        await waitFor(() => device.keyReadState === state);
        assert.equal(device.radioKey, null);
      }
      spy.mockImplementation((line) => line === "?KEY" ? Promise.resolve(true) : write(line));
      device.readRadioKey();
      await assert.rejects(device.provisionKey(replacement), /Wait for the board settings/);
      await waitFor(() => device.keyReadState === "timeout");
      assert.equal(device.radioKey, null);
      device.readRadioKey();
      await device.disconnect();
      assert.equal(device.radioKey, null);
      assert.equal(device.keyReadState, "idle");
      spy.mockRestore();
      await connectFake(device);
      assert.equal(device.radioKey, initial, "a new board does not inherit the previous board key");
    } finally {
      vi.restoreAllMocks();
      await device.disconnect();
    }
  });

  it("acks every event, decides a sprint on its two first crossings, freezes the calibration and records it", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    assert.equal(device.contract.ok, true);
    await waitFor(() => device.ppsEdges.length >= 2, 4000); // S1 inside PPS coverage
    assert.equal(timing.qualityFor("sprint").ok, true, JSON.stringify(timing.qualityFor("sprint").reasons));
    assert.equal(await timing.start("sprint", "car 7"), true, toasts.error.join(" | "));
    assert.equal(timing.lightColor, "green");
    assert.equal(await timing.start("sprint", "again"), false);
    assert.match(toasts.error.at(-1), /already running/);
    assert.equal(history.rows.length, 0);

    fake.crossing(a);
    fake.crossing(b, { offsetMs: 1000 });
    await waitFor(() => timing.run.verification === "verified");
    assert.equal(timing.run.closed, false);
    assert.equal(timing.lightColor, "green");
    assert.ok(timing.live.crossings.every((c) => c.confirmed));
    await waitFor(() => timing.run.durationNs != null, 4000);
    assert.equal(timing.run.calibration.method, "gps");
    const result = ms(timing.run.durationNs);
    assert.ok(result >= 1000 && result <= 1002, `result ${result}`);
    await waitFor(() => history.rows[0].durationNs != null);
    assert.equal(history.rows[0].verification, "verified");
    assert.equal(history.rows[0].durationNs, timing.run.durationNs);
    assert.equal(history.rows[0].note, "car 7");
    assert.equal(timing.armed, true);
    await timing.stop();
    assert.equal(timing.run.closed, true);
    assert.equal(timing.lightColor, "red");
    const exported = history.exportData().History[0];
    assert.equal(exported.Result, formatDuration(timing.resultNs));
    assert.equal(exported.Status, "Verified");
    assert.deepEqual(Object.keys(exported), ["Date", "Mode", "Note", "Result", "Status"]);

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
    assert.equal(again.timing.live.clockDisplay, timing.live.clockDisplay);
    assert.equal(again.settings.state.lastNote, "car 7");
  });

  it("keeps the manual Stop clock through delayed result calibration and reload", { timeout: 10000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    fake.setGps({ valid: false, fix: 0 });
    assert.equal(await timing.start("sprint"), true);
    const startTick = BigInt(fake.crossing(a));
    fake.crossing(b, { at: startTick + 400n * 16000n });
    await waitFor(() => timing.run.verification === "verified");
    assert.equal(timing.run.calibration, null);

    const now = Date.now();
    timing.live.startedAt = now - 12345;
    const nowSpy = vi.spyOn(Date, "now").mockReturnValue(now);
    try { await timing.stop(); } finally { nowSpy.mockRestore(); }
    assert.equal(timing.live.clockDisplay, "00:12.345");
    await waitFor(() => timing.run.durationNs != null, 4000);
    await waitFor(() => history.rows[0]?.durationNs != null);
    assert.equal(ms(timing.run.durationNs), 400);
    assert.equal(timing.live.clockDisplay, "00:12.345");
    assert.equal(history.exportData().History[0].Result, "00:00.400");

    await device.disconnect();
    const again = await boot({ keepStorage: true });
    assert.equal(again.timing.live.clockDisplay, "00:12.345");
    assert.equal(again.timing.resultNs, 400_000_000n);
  });

  it("ignores a finish before the first start (case F) and bundles a burst into one line", { timeout: 20000 }, async () => {
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

  it("a run decided just before a master reboot freezes with that boot's stored PPS edges", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    await waitFor(() => device.ppsEdges.length >= 2, 4000);
    assert.equal(await timing.start("sprint", "rb"), true, toasts.error.join(" | "));
    fake.setGps({ valid: false }); // no qualified edge after the result: the freeze waits
    fake.crossing(a);
    fake.crossing(b, { offsetMs: 1000 });
    await waitFor(() => timing.run.verification === "verified");
    fake.rebootMaster();
    await waitFor(() => timing.run.durationNs != null, 6000);
    assert.equal(timing.run.calibration.method, "gps-extrapolated");
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

  it("freezes the debounce at START - a later setting change or a restart never regroups crossings", { timeout: 20000 }, async () => {
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

  it("laps without a target closes on Stop and preserves verified results", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device, { periodicCheckpoints: false });
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    settings.setLapTarget("");
    assert.equal(await timing.start("laps", "en"), true, toasts.error.join(" | "));
    assert.equal(history.rows.length, 0);
    const t0 = BigInt(fake.crossing(a));
    await waitFor(() => timing.run.crossingTicks.length === 1);
    assert.equal(history.rows.length, 0);
    fake.crossing(a, { at: t0 + 400n * 16000n });
    await waitFor(() => history.rows.length === 1);
    const recordId = history.rows[0].id;
    for (const offset of [800, 1200]) fake.crossing(a, { at: t0 + BigInt(offset) * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 4);
    assert.equal(timing.run.verification, "verified"); // so far
    assert.equal(timing.run.closed, false);
    assert.equal(ms(timing.resultNs), 1200);
    await sleep(1300);
    const cpBefore = fake.cpRequests;
    assert.equal(await timing.stop(), true);
    assert.equal(timing.run.armed, false);
    assert.equal(timing.run.closed, true);
    assert.equal(fake.cpRequests, cpBefore);
    assert.equal(timing.run.verification, "verified");
    fake.crossing(a);
    await waitFor(() => fake.queueLength === 0);
    assert.equal(timing.run.crossingTicks.length, 4, "late captures cannot change a stopped run");
    await waitFor(() => timing.run.durationNs != null, 4000);
    assert.equal(ms(timing.run.durationNs), 1200);
    await waitFor(() => history.rows[0].durationNs != null);
    assert.equal(history.rows.length, 1);
    assert.equal(history.rows[0].id, recordId);
    assert.equal(ms(resultNs(history.rows[0])), 1200);
    const saved = await getAll(eventLog.getDb(), eventLog.STORE_RESULTS);
    assert.equal(saved.length, 1);
    assert.equal(saved[0].crossingTicks.length, 4);
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
    assert.match(timing.fault.reasons[0].reason, /Capture Loss/);
    assert.ok(!toasts.error.some((m) => /Capture Loss/.test(m)));
    await device.disconnect();
  });

  it("incomplete Start/Stop creates no result in storage or export, including after reload", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    assert.equal(await timing.start("sprint", "start only"), true, toasts.error.join(" | "));
    assert.equal(history.rows.length, 0);
    const startTick = fake.crossing(a);
    await waitFor(() => timing.live.crossings.length === 1);
    assert.equal(await timing.stop(), true);
    assert.equal(timing.run.closed, true);
    assert.equal(timing.run.startTick, String(startTick));
    fake.crossing(b);
    await waitFor(() => fake.queueLength === 0);
    assert.equal(timing.run.finishTick, null);
    assert.equal(timing.resultNs, null);
    assert.equal(history.rows.length, 0);
    timing.reset();
    for (const mode of ["sprint", "laps"]) {
      assert.equal(await timing.start(mode, "no crossings"), true, toasts.error.join(" | "));
      assert.equal(await timing.stop(), true);
      assert.equal(timing.run.closed, true);
      assert.equal(history.rows.length, 0);
    }
    await device.disconnect();
    const again = await boot({ keepStorage: true });
    assert.equal(again.timing.run.closed, true);
    assert.equal(again.timing.resultNs, null);
    assert.equal(again.history.rows.length, 0);
    assert.equal(again.history.exportData().History.length, 0);
    assert.deepEqual(await getAll(eventLog.getDb(), eventLog.STORE_RESULTS), []);
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

  it("Stop closes collection even if the master clock is unavailable or disconnected", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    assert.equal(await timing.start("laps", "clock"), true, toasts.error.join(" | "));
    const t0 = BigInt(fake.crossing(a));
    fake.crossing(a, { at: t0 + 400n * 16000n });
    await waitFor(() => timing.run.crossingTicks.length === 2);
    const readClock = vi.spyOn(device, "readClock").mockRejectedValue(new Error("no reply"));
    assert.equal(await timing.stop(), true);
    assert.equal(readClock.mock.calls.length, 0);
    assert.equal(timing.run.closed, true);
    assert.equal(timing.run.verification, "verified");
    assert.equal(ms(timing.resultNs), 400);
    readClock.mockRestore();
    timing.reset();

    assert.equal(await timing.start("laps", "offline"), true, toasts.error.join(" | "));
    fake.crossing(a);
    await waitFor(() => timing.live.crossings.length === 1);
    await device.disconnect();
    assert.equal(await timing.stop(), true);
    assert.equal(timing.run.armed, false);
    assert.equal(timing.run.closed, true);
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
    assert.match(timing.fault.reasons[0].reason, /Quarantined/);
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
    await waitFor(() => device.head != null);
    device.head.firstSeenAt -= 5200; // the same line has been re-sent for over 5 s
    assert.equal(timing.qualityFor("laps").ok, false);
    assert.match(timing.qualityFor("laps").reasons[0].reason, /keeps being retransmitted/);
    assert.equal(await timing.start("laps"), false);
    fake.setCorrupt(false);
    await waitFor(() => fake.queueLength === 0);
    await waitFor(() => timing.qualityFor("laps").ok, 3000);
    await device.disconnect();
  });

  it("gates the contract: a sensor-role board is never acked and cannot START; a protocol mismatch shows why", { timeout: 20000 }, async () => {
    const { device, settings, timing } = await boot();
    const fake = await connectFake(device);
    const [a] = [...fake.sensors.keys()];
    settings.setMapping(a, { role: "start" });
    fake.setRole("S");
    await waitFor(() => !device.contract.ok);
    assert.match(device.contract.reason, /Board is not a master/);
    const acks = device.stats.acks;
    fake.crossing(a);
    await sleep(400);
    assert.equal(device.stats.acks, acks);
    assert.ok(fake.queueLength >= 1);
    assert.equal(await timing.start("laps"), false);
    assert.match(toasts.error.at(-1), /Board is not a master/);
    fake.setRole("M");
    await waitFor(() => fake.queueLength === 0);
    assert.ok(device.stats.acks > acks);
    fake.setProto({ usb: 1 });
    await waitFor(() => !device.contract.ok);
    assert.match(device.contract.reason, /protocols do not match/);
    await device.disconnect();
  });

  it("logs X lines and boot reasons, warns on ver_drop, and reports storage failure", { timeout: 20000 }, async () => {
    const { device, settings, timing, history } = await boot();
    const fake = await connectFake(device);
    assert.ok(device.logEntries.some((e) => e.kind === "boot" && e.code === "power_on"));
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
    assert.ok(!device.alarms.some((x) => x.key === "storage"));
    assert.equal(device.durableError, "disk full");
    assert.equal(timing.run.durable, false);
    assert.equal(history.rows.length, 0);
    const t0 = BigInt(fake.crossing(a));
    await waitFor(() => timing.run.crossingTicks.length === 1); // acks keep flowing from memory
    assert.equal(history.rows.length, 0);
    fake.crossing(a, { at: t0 + 1000n * 16000n });
    await waitFor(() => history.rows[0]?.durable === false);
    timing.reset();
    assert.equal(await timing.start("laps"), true, toasts.error.join(" | "));
    assert.equal(timing.run.durable, false); // started after the failure
    await device.disconnect();
  });

});

describe("automatic sensor admission", () => {
  it("tracks connection states and clears the full-registry notice when admission resumes", async () => {
    const { device, settings } = await boot();
    await device.connect({ kind: "fake" });
    await waitFor(() => device.contract.ok && device.keyReadState !== "loading");
    const fake = device.fakeTransport(), node = [...fake.sensors.keys()][0];
    settings.setMapping(node, { role: "finish", note: "old sensor" });
    await device.handleLine("X registry_full");
    assert.match(device.admissionNotice, /Turn off an unused sensor/);
    await device.handleLine(`J ${node} WAIT`); assert.equal(device.admission[node].state, "WAIT");
    assert.equal(device.admissionNotice, "");
    await device.handleLine(`J ${node} SYNC`); assert.equal(device.admission[node].state, "SYNC");
    await device.handleLine(`J ${node} READY`); assert.equal(device.admission[node].state, "READY");
    assert.equal(settings.mappingOf(node).role, "finish");
    assert.equal(settings.mappingOf(node).note, "old sensor");
    await device.disconnect();
    assert.deepEqual(Object.keys(device.admission), []);
  });
});
