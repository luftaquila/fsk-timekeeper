// @vitest-environment jsdom
import "fake-indexeddb/auto";
import { webcrypto } from "node:crypto";
import { it, expect, vi } from "vitest";
import { createPinia, setActivePinia } from "pinia";
import { mount } from "@vue/test-utils";
vi.mock("../src/composables/useNotification", () => ({
  useNotification: () => ({ success() {}, error() {}, open() {} }),
}));
import * as eventLog from "../src/lib/eventLog";
import * as runLog from "../src/lib/runLog";
import * as backup from "../src/lib/backup";
import { getAll, put } from "../src/lib/idb";
import { useTimingStore } from "../src/stores/timing";
import { useHistoryStore } from "../src/stores/history";
import { createRun, evaluateRun, RUN_SCHEMA } from "../src/lib/engine";
import { historyRecord } from "../src/lib/historyPresentation";
import { RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "../src/lib/protocol";
import TimingView from "../src/views/TimingView.vue";

Object.defineProperty(globalThis, "crypto", { value: webcrypto, configurable: true });

async function boot(clear = true) {
  await runLog.flush();
  eventLog._reset();
  if (clear) await new Promise((resolve, reject) => {
    const r = indexedDB.deleteDatabase(eventLog.DB_NAME);
    r.onsuccess = resolve;
    r.onerror = () => reject(r.error);
  });
  const pinia = createPinia();
  setActivePinia(pinia);
  await eventLog.init();
  const history = useHistoryStore();
  return { pinia, history, timing: useTimingStore() };
}

it("shows every lap after Stop and preserves them through reload", async () => {
  const { pinia, timing, history } = await boot();
  const tick = ms => String(160000000n + BigInt(ms) * 16000n), now = Date.now();
  let seq = 0;
  const row = (node, kind, capture_seq, ms) => ({
    seq: ++seq, node_id: node, kind, capture_seq, end_seq: capture_seq,
    master_tick: tick(ms), end_tick: tick(ms), master_boot_id: "500", sensor_boot_id: "100",
    flags: 7, received_at: now,
  });
  const cps = [row("00000001", "checkpoint", 0, -1)];
  const r = createRun({
    mode: "laps", clock: { master_tick: tick(0), master_boot_id: "500" },
    mappings: [{ node_id: "00000001", role: "start" }],
    findCheckpoint: node => cps.find(c => c.node_id === node), currentSensorBoot: () => "100",
    lastSeq: 1, now,
  });
  const rows = [...cps, ...[1000, 3000, 4000].map((ms, i) => row("00000001", "capture", i + 1, ms))];
  const evaluated = evaluateRun(r, rows, { now });
  expect(evaluated.laps).toHaveLength(2);
  timing.run = evaluated.run;
  timing.live.crossings = evaluated.crossings;
  timing.live.laps = evaluated.laps;
  await timing.stop();
  const wrapper = mount(TimingView, { global: {
    plugins: [pinia], stubs: { RunSetupCard: true, RunControlCard: true, TimerCard: true, FaultReasons: true },
  } });
  const stats = Object.fromEntries(wrapper.findAll(".stats .stat").map(w => [w.find(".k").text(), w.find(".v").text()]));
  expect(stats.Laps).toBe("2");
  expect(timing.resultNs).toBe(3000000000n);
  expect(timing.lapRows.map(l => l.ns)).toEqual([2000000000n, 1000000000n]);
  expect(history.rows[0].crossingTicks).toHaveLength(3);
  wrapper.unmount();
  await runLog.flush();
  timing.discardRecords();
  const again = await boot(false);
  await again.timing.reevaluate();
  expect(again.timing.lapRows).toHaveLength(2);
  again.timing.discardRecords();
  await runLog.flush();
  eventLog._reset();
});

it("marks restored records durable and accepts reimport of the original unsaved backup", async () => {
  const { history } = await boot();
  const record = {
    schema: RUN_SCHEMA, radioProto: RADIO_PROTO_VERSION, usbProto: USB_PROTO_VERSION, runId: "volatile",
    mode: "sprint", verification: "pending", classification: "DNS", closed: true, armed: false,
    nodes: {}, cursor: 0, crossingTicks: [], startedAt: 1, durable: false,
  };
  const text = await backup.pack([], { runs: [record], events: [], pps: [], quarantine: [], log: [] });
  const file = { size: new TextEncoder().encode(text).length, text: async () => text };
  await history.importJson(file);
  const db = eventLog.getDb();
  const run = (await getAll(db, runLog.STORE_RUNS))[0];
  expect(run.durable).toBe(true);
  expect(historyRecord(history.rows[0]).Status).not.toContain("Not Saved");
  await history.importJson(file);
  expect(history.rows).toHaveLength(1);
  // Successful reimport also repairs a previously persisted false flag.
  await put(db, runLog.STORE_RUNS, { ...run, durable: false });
  await history.importJson(file);
  expect(runLog.get(record.runId).durable).toBe(true);
  const again = await boot(false);
  expect(again.history.rows[0].durable).toBe(true);
  eventLog._reset();
});

it("keeps the selected run durable after a restore and a later classification", async () => {
  await boot();
  const record = {
    schema: RUN_SCHEMA, radioProto: RADIO_PROTO_VERSION, usbProto: USB_PROTO_VERSION, runId: "selected",
    mode: "sprint", verification: "pending", classification: "DNS", closed: true, armed: false,
    nodes: {}, cursor: 0, crossingTicks: [], startedAt: 1, durable: false,
  };
  await runLog.save(record, { select: true });
  await runLog.flush();
  const { timing, history } = await boot(false);
  await timing.reevaluate();
  expect(timing.run.durable).toBe(false);
  // The backup made while the run could not be saved, restored once storage works again.
  const text = await history.recoveryJson();
  await history.importJson({ size: new TextEncoder().encode(text).length, text: async () => text });
  expect(timing.run.durable).toBe(true);
  expect(await timing.classify("DSQ")).toBe(true);
  await runLog.flush();
  const stored = (await getAll(eventLog.getDb(), runLog.STORE_RUNS))[0];
  expect([stored.classification, stored.durable]).toEqual(["DSQ", true]);
  expect(history.rows[0].durable).toBe(true);
  expect(runLog.get(record.runId).durable).toBe(true);
  timing.discardRecords();
  eventLog._reset();
});

it("a result voided after it was recorded reads the same in History and still freezes", { timeout: 15000 }, async () => {
  for (const mode of ["sprint", "laps"]) {
    const { timing, history } = await boot();
    const tick = ms => String(160000000n + BigInt(ms) * 16000n), now = Date.now();
    const row = (node, kind, capture_seq, ms) => ({ node_id: node, kind, capture_seq, end_seq: capture_seq,
      master_tick: tick(ms), end_tick: tick(ms), master_boot_id: "500", sensor_boot_id: "100", flags: 7 });
    let hseq = 0;
    const ingest = async (rows) => {
      timing.onEventRows((await eventLog.ingestLine({ rows, masterBootId: "500", hseq: ++hseq }, now)).rows);
      await runLog.flush();
    };
    const nodes = mode === "sprint" ? ["00000001", "00000002"] : ["00000001"];
    await ingest(nodes.map(node => row(node, "checkpoint", 0, -1)));
    timing.run = createRun({
      mode, clock: { master_tick: tick(0), master_boot_id: "500" },
      mappings: nodes.map((node_id, i) => ({ node_id, role: i ? "finish" : "start" })),
      findCheckpoint: node => eventLog.latestCheckpoint(node, "500", tick(0)), currentSensorBoot: () => "100",
      lastSeq: eventLog.getLastSeq() - nodes.length, now,
    });
    await ingest(mode === "sprint"
      ? [row("00000001", "capture", 1, 1000), row("00000002", "capture", 1, 3000)]
      : [1000, 3000, 5000].map((ms, i) => row("00000001", "capture", i + 1, ms)));
    expect(history.rows.map(r => r.verification)).toEqual(["verified"]);
    // A start capture out of order below a decided crossing: the crossings above it are withdrawn.
    await ingest([mode === "sprint" ? row("00000001", "capture", 2, 500) : row("00000001", "capture", 4, 2000)]);
    expect(timing.run.verification).toBe("invalid");
    expect(history.rows.map(r => [r.verification, r.crossingTicks.length])).toEqual([["invalid", mode === "sprint" ? 0 : 1]]);
    const [record] = history.rows;
    expect(record.calibration).not.toBeNull();
    expect(record.durationNs).toBeNull();
    expect(historyRecord(record)).toMatchObject({ Result: "-", Status: "Invalid" });
    timing.discardRecords();
  }
  eventLog._reset();
});
