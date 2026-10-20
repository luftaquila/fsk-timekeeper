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
import { createRun, evaluateRun } from "../src/lib/engine";
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
  await history.init();
  return { pinia, history, timing: useTimingStore() };
}

it("shows only confirmed laps after Stop and preserves them through reload", async () => {
  const { pinia, timing, history } = await boot();
  const tick = ms => String(160000000n + BigInt(ms) * 16000n), now = Date.now();
  let seq = 0;
  const row = (node, kind, capture_seq, ms) => ({
    seq: ++seq, node_id: node, kind, capture_seq, end_seq: capture_seq,
    master_tick: tick(ms), end_tick: tick(ms), master_boot_id: "500", sensor_boot_id: "100",
    flags: 7, received_at: now,
  });
  const cps = [row("00000001", "checkpoint", 0, -1), row("00000002", "checkpoint", 0, -1)];
  const r = createRun({
    mode: "laps", clock: { master_tick: tick(0), master_boot_id: "500" },
    mappings: cps.map(c => ({ node_id: c.node_id, role: "start" })),
    findCheckpoint: node => cps.find(c => c.node_id === node), currentSensorBoot: () => "100",
    lastSeq: 2, now,
  });
  // Sensor B's watermark confirms the first lap, but not A's last crossing.
  const rows = [...cps, ...[1000, 3000, 4000].map((ms, i) => row("00000001", "capture", i + 1, ms)),
    row("00000002", "checkpoint", 0, 3000)];
  const evaluated = evaluateRun(r, rows, { now });
  expect(evaluated.laps.map(l => l.confirmed)).toEqual([true, false]);
  timing.run = evaluated.run;
  timing.live.crossings = evaluated.crossings;
  timing.live.laps = evaluated.laps;
  await timing.stop();
  const wrapper = mount(TimingView, { global: {
    plugins: [pinia], stubs: { RunSetupCard: true, RunControlCard: true, TimerCard: true, FaultReasons: true },
  } });
  const stats = Object.fromEntries(wrapper.findAll(".stats .stat").map(w => [w.find(".k").text(), w.find(".v").text()]));
  expect(stats.Laps).toBe("1");
  expect(timing.resultNs).toBe(2000000000n);
  expect(timing.lapRows.map(l => l.ns)).toEqual([2000000000n]);
  expect(history.rows[0].crossingTicks).toHaveLength(2);
  wrapper.unmount();
  await runLog.flush();
  timing.discardRecords();
  const again = await boot(false);
  await again.timing.reevaluate();
  expect(again.timing.lapRows).toHaveLength(1);
  again.timing.discardRecords();
  await runLog.flush();
  eventLog._reset();
});

it("marks restored records durable and accepts reimport of the original unsaved backup", async () => {
  const { history } = await boot();
  const record = {
    schema: 3, radioProto: RADIO_PROTO_VERSION, usbProto: USB_PROTO_VERSION, runId: "volatile",
    mode: "sprint", verification: "pending", classification: "DNS", closed: true, armed: false,
    nodes: {}, cursor: 0, crossingTicks: [], createdAt: 1, durable: false,
  };
  const text = await backup.pack([], { results: [record], runs: [record], events: [], pps: [], quarantine: [], log: [] });
  const file = { size: new TextEncoder().encode(text).length, text: async () => text };
  await history.importJson(file);
  const db = eventLog.getDb();
  const stored = (await getAll(db, eventLog.STORE_RESULTS))[0];
  expect(stored.durable).toBe(true);
  expect(historyRecord(history.rows[0]).Status).not.toContain("Not Saved");
  const run = (await getAll(db, runLog.STORE_RUNS))[0];
  expect(run.durable).toBe(true);
  await history.importJson(file);
  expect(history.rows).toHaveLength(1);
  // Successful reimport also repairs a previously persisted false flag.
  await put(db, eventLog.STORE_RESULTS, { ...stored, durable: false });
  await put(db, runLog.STORE_RUNS, { ...run, durable: false });
  await history.importJson(file);
  expect(runLog.get(record.runId).durable).toBe(true);
  const again = await boot(false);
  expect(again.history.rows[0].durable).toBe(true);
  eventLog._reset();
});
