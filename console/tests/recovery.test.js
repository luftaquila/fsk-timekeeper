// Recovery integration: simulator -> evidence -> run/history -> portable JSON -> fresh DB.
import "fake-indexeddb/auto";
import { describe, it, vi } from "vitest";
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
const runLog = await import("../src/lib/runLog.js");
const backup = await import("../src/lib/backup.js");
const { getAll, put } = await import("../src/lib/idb.js");

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

async function boot({ keepStorage = false } = {}) {
  await eventLog.flush();
  await runLog.flush();
  eventLog._reset();
  if (!keepStorage) {
    localStorage.clear();
    await new Promise((resolve, reject) => {
      const request = indexedDB.deleteDatabase(eventLog.DB_NAME);
      request.onsuccess = resolve; request.onerror = () => reject(request.error);
    });
  }
  setActivePinia(createPinia());
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


describe("durable run recovery", () => {
  it("keeps pending GPS finalization through Clear, reload and a new run", { timeout: 15000 }, async () => {
    let { device, settings, timing, history } = await boot();
    const { fake, a, b } = await mapSprint(device, settings);
    fake.setGps({ ppb: 100000 });
    await waitFor(() => device.ppsEdges.length >= 3, 5000);
    fake.setGps({ valid: false, fix: 0 });
    await timing.start("sprint", "stable");
    const t = BigInt(fake.crossing(a)); fake.crossing(b, { at: t + 1600000000n });
    await waitFor(() => timing.run.verification === "verified");
    await timing.stop();
    const id = timing.run.runId, deadline = timing.run.finalizeAt;
    const before = history.exportData().History[0].Result;
    await timing.reset();
    await device.disconnect();
    ({ device, settings, timing, history } = await boot({ keepStorage: true }));
    assert.equal(timing.run, null);
    assert.equal(runLog.get(id).finalizeAt, deadline);
    await mapSprint(device, settings);
    await timing.start("sprint", "next");
    await timing.settleResults();
    assert.equal(history.rows.find(r => r.runId === id).calibration.method, "gps-extrapolated");
    assert.equal(history.exportData().History[0].Result, before);
    await device.disconnect(); await timing.reset();
    const again = await boot({ keepStorage: true });
    assert.equal(again.history.exportData().History[0].Result, before);
  });

  it("recovers a deadline that expired while closed using nominal timing without PPS", async () => {
    const { history } = await boot();
    const run = { schema: 3, usbProto: 7, radioProto: 17, runId: "expired", mode: "sprint", verification: "verified",
      classification: null, armed: false, closed: true, nodes: {}, cursor: 0, crossingTicks: [],
      startTick: "16000000", finishTick: "32000000", calibration: null, durationNs: null,
      finalizeAt: Date.now() - 5000, calibrationEdges: [], startedAt: 1, durable: true };
    await runLog.save(run); await history.upsert(run);
    const again = await boot({ keepStorage: true });
    assert.equal(again.history.rows[0].durationNs, "1000000000");
    assert.equal(again.history.rows[0].calibration.method, "nominal");
  });

  it("recovers active state without writing localStorage and exposes IndexedDB failure", async () => {
    const { device, settings, timing } = await boot();
    await mapSprint(device, settings);
    const original = localStorage.setItem;
    localStorage.setItem = () => { throw new DOMException("quota", "QuotaExceededError"); };
    // Keep settings unchanged so only the active-run persistence path is exercised.
    settings.state.lastNote = "";
    try {
      assert.equal(await timing.start("sprint", ""), true);
      assert.equal(device.durable, true);
      const id = timing.run.runId;
      await device.disconnect();
      localStorage.setItem = original;
      const again = await boot({ keepStorage: true });
      assert.equal(again.timing.run.runId, id);
      assert.equal(again.timing.armed, true);
      const spy = vi.spyOn(eventLog.getDb(), "transaction").mockImplementation(() => { throw new Error("disk full"); });
      await again.timing.stop();
      await runLog.flush(); spy.mockRestore();
      assert.equal(again.device.durable, false);
      assert.equal(again.timing.run.durable, false);
    } finally { localStorage.setItem = original; await device.disconnect(); }
  });

  it("serializes delete after queued writes and prevents later saves from reviving it", async () => {
    const { history } = await boot();
    const r = { schema:3, runId:"delete", mode:"sprint", verification:"verified", classification:null,
      closed:true, startTick:"16000000", finishTick:"32000000", crossingTicks:[], calibration:null,
      durationNs:"1000000000", cursor:0, startedAt:1, durable:true };
    await history.upsert(r);
    await Promise.all([history.upsert(r), history.removeRow(history.rows[0].id)]);
    await history.upsert(r);
    assert.equal(history.rows.length, 0);
    assert.equal((await getAll(eventLog.getDb(), eventLog.STORE_RESULTS)).length, 0);
    const again = await boot({ keepStorage:true });
    await again.history.upsert(r); assert.equal(again.history.rows.length, 0);
  });

  it("retains records when deletion fails", async () => {
    const { timing, history } = await boot();
    await timing.classify("DNS", "sprint", "keep");
    const id = history.rows[0].id;
    const spy = vi.spyOn(eventLog.getDb(), "transaction").mockImplementation(() => { throw new Error("delete failed"); });
    await assert.rejects(history.removeRow(id), /delete failed/);
    assert.equal(history.rows[0].id, id); spy.mockRestore();
  });
});

it("reacquires automatically released wake locks and releases late requests after disconnect", async () => {
  const doc = new EventTarget(); doc.visibilityState = "visible";
  const locks = [];
  const request = vi.fn(async () => { const lock = new EventTarget(); lock.released = false;
    lock.release = async () => { lock.released = true; lock.dispatchEvent(new Event("release")); };
    locks.push(lock); return lock; });
  vi.stubGlobal("document", doc); vi.stubGlobal("navigator", { wakeLock: { request } });
  const { device } = await boot();
  try {
    await connectFake(device); assert.equal(request.mock.calls.length, 1);
    doc.visibilityState = "hidden"; await locks[0].release();
    doc.visibilityState = "visible"; doc.dispatchEvent(new Event("visibilitychange")); await sleep(30);
    assert.equal(request.mock.calls.length, 2); assert.equal(locks[1].released, false);
    await device.disconnect(); assert.equal(locks[1].released, true);
    let resolve;
    request.mockImplementationOnce(() => new Promise(r => { resolve = r; }));
    await connectFake(device); await device.disconnect();
    const late = new EventTarget(); late.release = vi.fn(async () => {});
    resolve(late); await sleep(30); assert.equal(late.release.mock.calls.length, 1);
  } finally { await device.disconnect(); vi.unstubAllGlobals(); }
});

it("exports all records and imports atomically with exact values, deduplication and rollback", async () => {
  let { timing, history } = await boot();
  await timing.classify("DNS", "sprint", "sprint"); await timing.reset();
  await timing.classify("DNF", "laps", "laps");
  const text = await history.recoveryJson(); const parsed = await backup.unpack(text);
  assert.equal(parsed.results.length, 2);
  ({ timing, history } = await boot());
  const file = { size: text.length, text: async () => text };
  assert.equal((await history.importJson(file)).added, 2);
  assert.equal(history.rows.length, 2); assert.equal(timing.armed, false);
  assert.equal((await history.importJson(file)).added, 0);
  assert.equal(history.rows.length, 2);
  const conflict = runLog.clone(parsed); conflict.results[0].note = "different";
  const bad = await backup.pack([], conflict);
  await assert.rejects(history.importJson({ size:bad.length, text:async()=>bad }), /Conflicting record/);
  assert.equal(history.rows.length, 2);
  const corrupt = text.replace('"sprint"', '"altered"');
  await assert.rejects(backup.unpack(corrupt));
  const roundtrip = await backup.unpack(await history.recoveryJson());
  assert.deepEqual(roundtrip.results.map(r=>r.classification).sort(), ["DNF","DNS"]);
});

it("round-trips frozen GPS ticks, evidence and unfinished snapshots into a populated database", async () => {
  let { device, settings, timing, history } = await boot();
  const { fake, a, b } = await mapSprint(device, settings);
  await timing.start("sprint", "exact");
  const tick = BigInt(fake.crossing(a)); fake.crossing(b, { at: tick + 16008000n });
  await waitFor(() => timing.run.verification === "verified");
  await timing.stop(); await timing.settleResults();
  const id = timing.run.runId;
  const expected = runLog.clone(history.rows[0]);
  await timing.reset(); await timing.start("laps", "unfinished");
  const text = await history.recoveryJson();
  const data = await backup.unpack(text);
  assert.ok(data.events.length > 0); assert.ok(data.pps.length > 0);
  assert.ok(data.runs.some(r => r.armed));
  await assert.rejects(history.importJson({ size:text.length, text:async()=>text }), /Stop measurement/);
  await device.disconnect(); await timing.reset();
  ({ timing, history } = await boot());
  // Existing evidence has unrelated local sequence numbers.
  const unrelated = { ...data.events[0], master_boot_id:"9999999", hseq:9999 };
  delete unrelated.seq;
  await eventLog.ingestLine({ rows:[unrelated], masterBootId:"9999999", hseq:9999 });
  const before = await getAll(eventLog.getDb(), eventLog.STORE_EVENTS);
  const result = await history.importJson({ size:text.length, text:async()=>text });
  assert.equal(result.added, 1); assert.equal(timing.armed, false);
  const actual = history.rows.find(r => r.runId === id);
  for (const key of ["durationNs", "calibration", "startTick", "finishTick", "verification"]) assert.deepEqual(actual[key], expected[key]);
  const restored = runLog.all().find(r=>r.note === "unfinished");
  assert.equal(restored.closed, true); assert.equal(restored.armed, false);
  const events = await getAll(eventLog.getDb(), eventLog.STORE_EVENTS);
  assert.equal(events.length, before.length + data.events.length);
  assert.ok(restored.cursor > data.runs.find(r=>r.note === "unfinished").cursor);
  await history.importJson({ size:text.length, text:async()=>text });
  assert.equal((await getAll(eventLog.getDb(), eventLog.STORE_EVENTS)).length, events.length);
  const after = await backup.unpack(await history.recoveryJson());
  assert.equal(after.results.find(r=>r.runId===id).durationNs, expected.durationNs);
});

it("rolls back earlier inserted evidence when a later import request aborts", async () => {
  const { history } = await boot();
  const e = { seq:1, kind:"capture", master_boot_id:"500", sensor_boot_id:"600", node_id:"ABCDEF01",
    capture_seq:1, master_tick:"16000000", hseq:1, received_at:1 };
  const pps = { master_boot_id:"500", tick:"16000000", seg:1, n:0, received_at:1 };
  await put(eventLog.getDb(), eventLog.STORE_PPS, pps);
  const data = { results:[], runs:[], events:[e], pps:[{...pps,n:5}], quarantine:[], log:[] };
  const text = await backup.pack([], data);
  await assert.rejects(history.importJson({size:text.length,text:async()=>text}), /Conflicting pps/);
  assert.equal((await getAll(eventLog.getDb(),eventLog.STORE_EVENTS)).length,0);
  assert.equal(eventLog.since(0).length,0);
});

it("exports memory-only quarantine and result data after a storage failure", async () => {
  const { timing, history } = await boot();
  await timing.classify("DNS", "sprint", "memory backup");
  eventLog.reportWriteFailure(new Error("disk full"));
  await eventLog.ingestQuarantine({raw:"bad evidence", reason:"invalid", node:null, hseq:77, masterBootId:null});
  const text = await history.recoveryJson();
  const data = await backup.unpack(text);
  assert.equal(data.results.length, 1);
  assert.equal(data.results[0].durable, false);
  assert.equal(data.runs[0].durable, false);
  assert.equal(data.quarantine.length, 1);
  const restored = await boot();
  await restored.history.importJson({size:text.length,text:async()=>text});
  assert.equal(restored.history.rows.length, 1);
  assert.equal((await eventLog.listQuarantine()).length, 1);
});

it("restores result-only deletions explicitly and remaps device-log evidence watermarks", async () => {
  const { history } = await boot();
  const row = {schema:3,runId:"archived",mode:"sprint",verification:"verified",classification:null,
    closed:true,startTick:"16000000",finishTick:"32000000",durationNs:"1000000000",cursor:0,startedAt:1};
  await history.upsert(row);
  const data = await backup.unpack(await history.recoveryJson());
  data.events = [{seq:100,kind:"quarantine",master_boot_id:"5",node_id:"*",hseq:1,raw:"bad",received_at:1}];
  data.log = [{at:1,kind:"state",code:"radio_history_clear",throughSeq:100,text:""}];
  const text = await backup.pack([],data);
  await history.removeRow(history.rows[0].id);assert.equal(runLog.isDeleted(row.runId),true);
  await history.importJson({size:text.length,text:async()=>text});
  assert.equal(runLog.isDeleted(row.runId),false);
  assert.equal(history.rows.length,1);
  const [log] = await getAll(eventLog.getDb(),eventLog.STORE_LOG);
  const [event] = await getAll(eventLog.getDb(),eventLog.STORE_EVENTS);
  assert.equal(log.throughSeq,event.seq);
});

it("upgrades an existing version-2 database and migrates its active localStorage run", async () => {
  await boot(); await runLog.flush(); eventLog._reset();
  await new Promise((resolve,reject)=>{
    const req=indexedDB.deleteDatabase(eventLog.DB_NAME);
    req.onsuccess=resolve;req.onerror=()=>reject(req.error);
  });
  const oldDb = await new Promise((resolve,reject)=>{
    const req=indexedDB.open(eventLog.DB_NAME,2);
    req.onupgradeneeded=()=>{
      const db=req.result;
      db.createObjectStore("events",{keyPath:"seq",autoIncrement:true})
        .createIndex("row",["master_boot_id","node_id","sensor_boot_id","kind","capture_seq","master_tick"],{unique:true});
      db.createObjectStore("results",{keyPath:"id",autoIncrement:true}).createIndex("runId","runId",{unique:true});
      db.createObjectStore("pps",{keyPath:["master_boot_id","tick"]}).createIndex("received_at","received_at");
      db.createObjectStore("quarantine",{keyPath:"id",autoIncrement:true}).createIndex("line","line",{unique:true});
      db.createObjectStore("log",{keyPath:"id",autoIncrement:true}).createIndex("at","at");
    };
    req.onsuccess=()=>resolve(req.result);req.onerror=()=>reject(req.error);
  });
  const row={schema:3,runId:"pre-upgrade",mode:"sprint",verification:"pending",classification:"DNS",closed:true,
    armed:false,nodes:{},cursor:0,crossingTicks:[],usbProto:6,radioProto:15,createdAt:1,durable:true};
  await put(oldDb,"results",row);oldDb.close();
  localStorage.setItem("tk.run.v3",JSON.stringify(row));
  const {timing,history}=await boot({keepStorage:true});
  await runLog.flush();
  assert.equal(eventLog.getDb().version,3);
  assert.equal(timing.run.runId,row.runId);
  assert.equal(history.rows.length,1);
  assert.equal(runLog.active().runId,row.runId);
  assert.equal(localStorage.getItem("tk.run.v3"),null);
});

const reviewedRun = (runId, extra = {}) => ({ schema:3, usbProto:6, radioProto:15, runId, mode:"sprint", verification:"verified",
  classification:null, armed:false, closed:true, nodes:{}, cursor:0, throughSeq:0, crossingTicks:[], masterBootId:"500",
  startTick:"16000000", finishTick:"32000000", calibration:null, durationNs:null, startedAt:1, durable:true, ...extra });
const recoveryFile = text => ({ size:new TextEncoder().encode(text).length, text:async()=>text });

it("preserves operator classifications and explicit Clear while detached results finalize", async () => {
  for (const [before, after] of [[null,"DSQ"],["DSQ",null],["DNF","DNS"]]) {
    let { timing, history } = await boot();
    const run = reviewedRun("late-freeze", {classification:before,finalizeAt:Date.now()+80,calibrationEdges:[]});
    await runLog.save(run,{select:true});await history.upsert(run);await timing.reset();
    await timing.classify("DNS","sprint","next attempt");
    await history.classify(run.runId,after);
    await timing.settleResults();
    const row = history.rows.find(r=>r.runId===run.runId);
    assert.equal(row.classification,after);
    assert.equal(row.calibration.method,"nominal");
    assert.equal(row.durationNs,"1000000000");
    ({history}=await boot({keepStorage:true}));
    assert.equal(history.rows.find(r=>r.runId===run.runId).classification,after);
  }
});

const recoveryPps = () => [0,1,1000].map(n=>({ master_boot_id:"500",tick:String(16001600n+BigInt(n)*16001600n),
  seg:1,n,utc:1800000000+n,received_at:Date.now() }));

it("freezes imported ongoing laps with file PPS, local PPS, or nominal timing, and never recalibrates them", async () => {
  for (const source of ["file","local","none","other-boot"]) {
    let { history } = await boot();
    const run = reviewedRun(`laps-${source}`,{mode:"laps",armed:true,closed:false,startTick:null,finishTick:null,totalValid:true,
      crossingTicks:["16001600","16017601600"]});
    await runLog.save(run);await history.upsert(run);
    if (source==="file") for (const edge of recoveryPps()) await put(eventLog.getDb(),eventLog.STORE_PPS,edge);
    const text=await history.recoveryJson();
    ({history}=await boot());
    if (["local","other-boot"].includes(source)) for (const edge of recoveryPps()) {
      await put(eventLog.getDb(),eventLog.STORE_PPS,{...edge,master_boot_id:source==="local"?"500":"999"});
    }
    await history.importJson(recoveryFile(text));
    const expected=["file","local"].includes(source)?"1000000000000":"1000100000000";
    assert.equal(history.rows[0].durationNs,expected);
    const restored=runLog.get(run.runId);
    assert.equal(restored.durationNs,expected);assert.equal(restored.armed,false);assert.equal(restored.closed,true);
    assert.deepEqual(restored.calibration,history.rows[0].calibration);
    assert.equal(restored.calibration.method,["file","local"].includes(source)?"gps":"nominal");
    // Even if PPS becomes available later, reimport and restart preserve the first freeze.
    for (const edge of recoveryPps()) await put(eventLog.getDb(),eventLog.STORE_PPS,edge);
    await history.importJson(recoveryFile(text));assert.equal(history.rows[0].durationNs,expected);
    ({history}=await boot({keepStorage:true}));assert.equal(history.rows[0].durationNs,expected);
    const exported=await history.recoveryJson();
    ({history}=await boot());await history.importJson(recoveryFile(exported));
    assert.equal(history.rows[0].durationNs,expected);
  }
});

it("preserves existing frozen calibration when imported without PPS", async () => {
  const {freezeCalibration}=await import("../src/lib/calibration.js");
  let {history}=await boot();
  const run=reviewedRun("frozen-without-pps",{mode:"laps",totalValid:true,startTick:null,finishTick:null,
    crossingTicks:["16001600","16017601600"],durationNs:"1000000000000"});
  run.calibration=freezeCalibration(recoveryPps(),run.crossingTicks);
  await runLog.save(run);await history.upsert(run);
  const text=await history.recoveryJson();
  ({history}=await boot());await history.importJson(recoveryFile(text));
  assert.deepEqual(history.rows[0].calibration,run.calibration);
  assert.deepEqual(runLog.get(run.runId).calibration,run.calibration);
  assert.equal(history.rows[0].durationNs,"1000000000000");
});

const reviewedEvent = n => ({seq:n,kind:"capture",master_boot_id:"500",sensor_boot_id:"600",node_id:"ABCDEF01",
  capture_seq:n,master_tick:String(BigInt(n)*16000000n),hseq:n,received_at:1});

it("preserves exact evidence membership and source order across partial overlap, reload and repeated exports", async () => {
  for (const existingOrder of [[2],[3,2],[2,99,3]]) {
    let {history}=await boot();
    for (const n of existingOrder) {
      const row=reviewedEvent(n);delete row.seq;
      await eventLog.ingestLine({rows:[row],masterBootId:"500",hseq:n},1);
    }
    const run=reviewedRun("membership",{cursor:1,throughSeq:3,nodes:{ABCDEF01:{cpSeq:1}}});
    const data={results:[{...run,cursorSeq:1}],runs:[run],events:[1,2,3].map(reviewedEvent),pps:[],quarantine:[],log:[]};
    const text=await backup.pack([],data);
    await history.importJson(recoveryFile(text));
    const assertMembers=async()=>{
      assert.deepEqual((await eventLog.rowsFor(history.rows[0])).map(e=>e.capture_seq),[2,3]);
      assert.deepEqual((await eventLog.rowsFor(runLog.get(run.runId))).map(e=>e.capture_seq),[2,3]);
      assert.ok(history.rows[0].throughSeq>history.rows[0].cursorSeq);
    };
    await assertMembers();await history.importJson(recoveryFile(text));await assertMembers();
    ({history}=await boot({keepStorage:true}));await assertMembers();
    const exported=await history.recoveryJson();
    ({history}=await boot());await history.importJson(recoveryFile(exported));await assertMembers();
    // All rows are retained; the reference list, not min/max, selects the run's evidence.
    assert.equal((await getAll(eventLog.getDb(),eventLog.STORE_EVENTS)).length,existingOrder.includes(99)?4:3);
  }
});

it("remaps imported log-clear membership without clearing unrelated or later evidence", async () => {
  const {history}=await boot();
  const now=Date.now();
  const checkpoint=n=>({...reviewedEvent(n),kind:"checkpoint",node_id:String(n).padStart(8,"0"),ev_seq:n,received_at:now,
    radio_history:{faults:[1,0,0,0,0,0],recovery:{count:0,failures:0,reason:0,result:0}}});
  for (const n of [2,99]) {const row=checkpoint(n);delete row.seq;await eventLog.ingestLine({rows:[row],masterBootId:"500",hseq:n},now);}
  const data={results:[],runs:[],events:[1,2,3].map(checkpoint),pps:[],quarantine:[],
    log:[{at:1,kind:"state",code:"radio_history_clear",throughSeq:2,text:""}]};
  await history.importJson(recoveryFile(await backup.pack([],data)));
  const [mark]=await getAll(eventLog.getDb(),eventLog.STORE_LOG);
  assert.deepEqual((await eventLog.rowsFor(mark)).map(e=>e.capture_seq),[1,2]);
  assert.equal(mark.evidenceSeqs.includes(2),false); // unrelated capture 99
  const loggedNodes=async()=>(await getAll(eventLog.getDb(),eventLog.STORE_LOG))
    .filter(e=>e.code==="radio_history").map(e=>e.nodeId).sort();
  assert.deepEqual(await loggedNodes(),["00000003","00000099"]);
  const restored=await boot({keepStorage:true});await restored.device.loadLog();
  assert.deepEqual(await loggedNodes(),["00000003","00000099"]);
});

it("clears all records and caches, survives reload, and records again", async () => {
  let { device, settings, timing, history } = await boot();
  const { fake, a, b } = await mapSprint(device, settings);
  await timing.start("sprint", "erase everything");
  await assert.rejects(history.clearAll(), /Stop measurement/);
  const tick = BigInt(fake.crossing(a)); fake.crossing(b, { at: tick + 16000000n });
  await waitFor(() => timing.run.verification === "verified");
  await timing.stop();
  await eventLog.ingestQuarantine({raw:"invalid", reason:"invalid", node:null, hseq:999, masterBootId:"500"});
  localStorage.setItem("tk.run.v3", "legacy");
  localStorage.setItem("tk.run.v2", "legacy");
  localStorage.setItem("unrelated-setting", "keep");
  assert.ok(history.rows.length);
  assert.ok(eventLog.since(0).length);
  const clearing = history.clearAll();
  assert.equal(await timing.classify("DNS", "sprint", "during deletion"), false);
  assert.equal(await device.connect({kind:"fake"}), false);
  await clearing;
  assert.equal(device.connected, false);
  assert.equal(timing.run, null);
  assert.equal(history.rows.length, 0);
  assert.equal(eventLog.since(0).length, 0);
  assert.equal(device.logEntries.length, 0);
  assert.equal(device.ppsEdges.length, 0);
  assert.equal(device.quarantine, null);
  assert.equal(localStorage.getItem("tk.run.v3"), null);
  assert.equal(localStorage.getItem("tk.run.v2"), null);
  assert.equal(localStorage.getItem("unrelated-setting"), "keep");
  for (const name of eventLog.getDb().objectStoreNames) assert.deepEqual(await getAll(eventLog.getDb(), name), [], name);
  const exported = await backup.unpack(await history.recoveryJson());
  for (const key of ["results","runs","events","pps","quarantine","log"]) assert.deepEqual(exported[key], [], key);
  // New measurements work without a page reload, including calibration finalization.
  const next = await mapSprint(device, settings);
  await timing.start("sprint", "new measurement");
  const nextTick = BigInt(next.fake.crossing(next.a)); next.fake.crossing(next.b, {at:nextTick+16000000n});
  await waitFor(() => timing.run.verification === "verified");
  await timing.stop();
  await timing.settleResults();
  assert.equal(history.rows.length, 1);
  assert.ok(history.rows[0].calibration);
  await history.clearAll();
  // Empty History must still allow clearing remaining evidence.
  await eventLog.ingestLine({rows:[reviewedEvent(1)],masterBootId:"500",hseq:1});
  await history.clearAll();
  ({device, timing, history} = await boot({keepStorage:true}));
  await device.loadLog();
  assert.equal(timing.run, null);
  assert.equal(history.rows.length, 0);
  assert.equal(device.logEntries.length, 0);
  const restored = await backup.unpack(await history.recoveryJson());
  for (const key of ["results","runs","events","pps","quarantine","log"]) assert.deepEqual(restored[key], [], key);
});

it("clears stale disk records and memory fallback after a storage failure", async () => {
  let {timing, history, device} = await boot();
  await timing.classify("DNS", "sprint", "on disk");
  eventLog.reportWriteFailure(new Error("storage lost"));
  await timing.reset();
  await timing.classify("DNF", "sprint", "in memory");
  await device.handleLine("X badcmd");
  await eventLog.ingestQuarantine({raw:"bad",reason:"invalid",node:null,hseq:1,masterBootId:null});
  await history.clearAll();
  const data = await backup.unpack(await history.recoveryJson());
  for (const key of ["results","runs","events","pps","quarantine","log"]) assert.deepEqual(data[key], [], key);
  ({timing, history, device} = await boot({keepStorage:true}));
  await device.loadLog();
  assert.equal(history.rows.length, 0);
  assert.equal(timing.run, null);
  assert.equal(device.logEntries.length, 0);
});

it("retains data and releases the deletion lock when clearing fails", async () => {
  const {timing, history, device} = await boot();
  await timing.classify("DNS", "sprint", "keep on failure");
  const id = history.rows[0].runId;
  const spy = vi.spyOn(eventLog.getDb(), "transaction").mockImplementation(() => {throw new Error("clear failed");});
  try { await assert.rejects(history.clearAll(), /clear failed/); }
  finally { spy.mockRestore(); }
  assert.equal(history.clearing, false);
  assert.equal(device.clearingData, false);
  assert.equal(history.rows[0].runId, id);
  assert.equal(timing.run.runId, id);
  assert.equal((await getAll(eventLog.getDb(), eventLog.STORE_RESULTS))[0].runId, id);
});

it("reads completed v14/USB5 results without changing their time or classification", async () => {
  const { decodeRun } = await import("../src/lib/run-codec.js");
  const run = reviewedRun("prior-radio", { usbProto: 5, radioProto: 14, classification: "DNF", durationNs: "1234567890" });
  const decoded = decodeRun(JSON.stringify(run));
  assert.equal(decoded.protocolChanged, false);
  assert.deepEqual(decoded.run, run);
  const open = decodeRun(JSON.stringify({ ...run, closed: false, armed: true }));
  assert.equal(open.protocolChanged, true); assert.equal(open.run.closed, true); assert.equal(open.run.armed, false);
});
