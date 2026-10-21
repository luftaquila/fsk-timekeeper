/* Portable JSON recovery. Local DB keys are remapped; imported runs never arm hardware. */
import * as eventLog from "./eventLog";
import * as runLog from "./runLog";
import * as deviceLog from "./deviceLog";
import { transaction, request, done } from "./idb";
import { enqueue } from "./storageQueue";
import { rowKey } from "./protocol";
import { resultNs, lapsNs, resultTicks, frozenDurationNs } from "./results";
import { freezeCalibration } from "./calibration";
import { RUN_SCHEMA, isRecord } from "./engine";

export const MAX_BACKUP_BYTES = 64 * 1024 * 1024;
const stores = { runs: runLog.STORE_RUNS, events: eventLog.STORE_EVENTS,
  pps: eventLog.STORE_PPS, quarantine: eventLog.STORE_QUARANTINE, log: eventLog.STORE_LOG };
const fail = message => { throw new Error(message); };
const object = v => v && typeof v === "object" && !Array.isArray(v);
const uint = v => Number.isSafeInteger(v) && v >= 0;
const tick = v => typeof v === "string" && /^(0|[1-9]\d{0,19})$/.test(v) && BigInt(v) <= 18446744073709551615n;
const optionalTick = v => v == null || tick(v);
const decimal = v => typeof v === "string" && /^-?\d{1,80}$/.test(v);
const localFields = new Set(["id", "seq", "cursor", "throughSeq", "evidenceSeqs", "cpSeq", "imported", "durable"]);
export function canonical(value) {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (object(value)) return `{${Object.keys(value).filter(k => value[k] !== undefined).sort().map(k => `${JSON.stringify(k)}:${canonical(value[k])}`).join(",")}}`;
  return JSON.stringify(value) ?? "null";
}
function portable(value) {
  if (Array.isArray(value)) return value.map(portable);
  if (!object(value)) return value;
  return Object.fromEntries(Object.entries(value).filter(([k]) => !localFields.has(k)).map(([k, v]) => [k, portable(v)]));
}
function equivalent(a, b, run = false) {
  const normalize = v => portable(run ? { ...v, armed: false, closed: true } : v);
  return canonical(normalize(a)) === canonical(normalize(b));
}
export async function checksum(value) {
  const bytes = new TextEncoder().encode(canonical(value));
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), n => n.toString(16).padStart(2, "0")).join("");
}

export function snapshot(memoryPps = []) {
  return enqueue(async () => {
    const db = eventLog.getDb();
    if (!db) return runLog.clone({ runs: runLog.all(), events: eventLog.since(0),
      pps: memoryPps, quarantine: await eventLog.listQuarantine(), log: await deviceLog.listEntries() });
    const tx = transaction(db, Object.values(stores));
    const finished = done(tx);
    const arrays = await Promise.all(Object.values(stores).map(store => request(tx.objectStore(store).getAll())));
    await finished;
    const data = Object.fromEntries(Object.keys(stores).map((key, i) => [key, arrays[i]]));
    data.runs = data.runs.filter(r => !r.deleted);
    return data;
  });
}

export async function pack(History, recovery) {
  const file = { format: "fsk-timekeeper", version: 2, exportedAt: new Date().toISOString(), History, recovery,
    sha256: await checksum({ History, recovery }) };
  const text = JSON.stringify(file, null, 2);
  if (new TextEncoder().encode(text).length > MAX_BACKUP_BYTES) fail("JSON exceeds the 64 MB limit.");
  return text;
}

function validateTree(v, depth = 0) {
  if (depth > 24) fail("JSON nesting is too deep.");
  if (typeof v === "number" && !Number.isFinite(v)) fail("Invalid number.");
  if (!v || typeof v !== "object") return;
  for (const [key, item] of Object.entries(v)) {
    if (["__proto__", "prototype", "constructor"].includes(key)) fail("Invalid JSON field.");
    validateTree(item, depth + 1);
  }
}
function validateRun(r) {
  if (!object(r) || typeof r.runId !== "string" || !r.runId.length || r.runId.length > 160 ||
      !["sprint", "laps"].includes(r.mode) || !["pending", "verified", "invalid"].includes(r.verification) ||
      (r.classification != null && !["DNS", "DNF", "DSQ"].includes(r.classification))) fail("Invalid run record.");
  if (r.schema !== RUN_SCHEMA) fail("Unsupported run schema.");
  for (const key of ["startTick", "finishTick", "boundaryTick"]) if (!optionalTick(r[key])) fail("Invalid run timestamp.");
  if (r.durationNs != null && (!decimal(r.durationNs) || BigInt(r.durationNs) < 0n)) fail("Invalid result duration.");
  if (r.masterBootId != null && !tick(r.masterBootId)) fail("Invalid master session.");
  if (r.crossingTicks != null && (!Array.isArray(r.crossingTicks) || r.crossingTicks.some(t => !tick(t)))) fail("Invalid lap timestamps.");
  if (r.recordedAt != null && !uint(r.recordedAt)) fail("Invalid record time.");
  if (r.throughSeq != null && !uint(r.throughSeq)) fail("Invalid evidence cursor.");
  if (!object(r.nodes) || typeof r.armed !== "boolean" || typeof r.closed !== "boolean" || !uint(r.cursor)) fail("Invalid run snapshot.");
  if (r.calibration) {
    const c = r.calibration;
    if (c.version !== 1 || !["nominal", "gps", "gps-extrapolated"].includes(c.method) || !object(c.points)) fail("Invalid calibration.");
    for (const [t, p] of Object.entries(c.points)) {
      if (!tick(t) || !object(p) || !decimal(p.num) || !decimal(p.den) || BigInt(p.den) <= 0n) fail("Invalid calibration point.");
    }
  }
  try { resultNs(r); lapsNs(r); } catch { fail("Invalid timing data."); }
}

function validateEvidenceRefs(row) {
  if (row.evidenceSeqs == null) return;
  const refs = row.evidenceSeqs;
  if (!Array.isArray(refs) || refs.length > 500000 || refs.some(n => !uint(n) || n === 0) ||
      new Set(refs).size !== refs.length) fail("Invalid evidence references.");
}

// Import closes unfinished snapshots: frozen with same-session PPS, else nominal timing.
function finalizeImported(row, edges, old, now) {
  if (row.calibration || !resultTicks(row).length) return row;
  const withoutCalibration = r => ({ ...r, calibration: null, durationNs: null });
  const reuse = old?.calibration && equivalent(withoutCalibration(old), withoutCalibration(row), true);
  const calibration = reuse ? old.calibration : freezeCalibration(edges.filter(e => e.master_boot_id === row.masterBootId), resultTicks(row), now);
  const next = { ...row, calibration };
  next.durationNs = frozenDurationNs(next);
  return next;
}

export async function unpack(text) {
  if (typeof text !== "string" || new TextEncoder().encode(text).length > MAX_BACKUP_BYTES) fail("JSON exceeds the 64 MB limit.");
  let file;
  try { file = JSON.parse(text); } catch { fail("Invalid JSON file."); }
  validateTree(file);
  if (file?.format !== "fsk-timekeeper" || file.version !== 2 || !object(file.recovery) || !Array.isArray(file.History)) fail("Unsupported recovery JSON format.");
  const data = file.recovery;
  for (const name of Object.keys(stores)) if (!Array.isArray(data[name]) || data[name].length > 500000) fail(`Invalid ${name} records.`);
  if (file.sha256 !== await checksum({ History: file.History, recovery: data })) fail("JSON integrity check failed.");
  const ids = new Set();
  for (const r of data.runs) {
    validateRun(r);
    validateEvidenceRefs(r);
    if (ids.has(r.runId)) fail("Duplicate run ID in file.");
    ids.add(r.runId);
  }
  const seqs = new Set();
  for (const e of data.events) {
    if (!object(e) || !uint(e.seq) || e.seq === 0 || seqs.has(e.seq) ||
        !["capture", "checkpoint", "loss", "quarantine"].includes(e.kind) ||
        !(e.kind === "quarantine" ? optionalTick(e.master_boot_id) : tick(e.master_boot_id)) || typeof e.node_id !== "string") fail("Invalid evidence row.");
    if (e.kind !== "quarantine" && (!tick(e.sensor_boot_id) || !tick(e.master_tick) || !uint(e.capture_seq))) fail("Invalid evidence timestamp.");
    seqs.add(e.seq);
  }
  for (const e of data.pps) {
    if (!object(e) || !tick(e.master_boot_id) || !tick(e.tick) || !uint(e.n) || !uint(e.seg) || e.seg < 1 || !uint(e.received_at)) fail("Invalid PPS edge.");
  }
  for (const r of data.quarantine) if (!object(r) || typeof r.line !== "string" || typeof r.raw !== "string") fail("Invalid quarantine row.");
  for (const r of data.log) {
    if (!object(r) || !uint(r.at) || typeof r.kind !== "string") fail("Invalid device log row.");
    validateEvidenceRefs(r);
  }
  return data;
}

const evidenceKey = r => r.kind === "quarantine" ? `q:${r.master_boot_id}:${r.hseq}` : rowKey(r);

// Checked while merging in one transaction: any conflict rolls every store back.
export function merge(data) {
  return enqueue(async () => {
    const db = eventLog.getDb();
    if (!db) fail("Browser storage is unavailable. Import was not applied.");
    const tx = transaction(db, Object.values(stores), "readwrite");
    const finished = done(tx);
    // Install the rejection handler before doing request-level work.
    finished.catch(() => {});
    const addedEvents = [], addedRuns = [];
    let added = 0, skipped = 0;
    try {
      const values = await Promise.all(Object.values(stores).map(s => request(tx.objectStore(s).getAll())));
      const existing = Object.fromEntries(Object.keys(stores).map((k, i) => [k, values[i]]));
      const runsById = new Map(existing.runs.map(r => [r.runId, r]));
      const edges = [...new Map([...existing.pps, ...data.pps].map(e => [`${e.master_boot_id}:${e.tick}`, e])).values()];
      const now = Date.now();
      data = { ...data, runs: data.runs.map(r => finalizeImported(r, edges, runsById.get(r.runId), now)) };
      for (const r of data.runs) {
        const old = runsById.get(r.runId);
        if (old && !old.deleted && !equivalent(old, r, true)) fail(`Conflicting run: ${r.runId}. Nothing was imported.`);
      }
      const eventIds = new Map(existing.events.map(r => [evidenceKey(r), r]));
      const remap = new Map();
      for (const e of [...data.events].sort((a, b) => a.seq - b.seq)) {
        const key = evidenceKey(e), old = eventIds.get(key);
        if (old) {
          // Reception time is local; immutable evidence must agree.
          if (!equivalent({ ...old, received_at: 0 }, { ...e, received_at: 0 })) fail("Conflicting evidence. Nothing was imported.");
          remap.set(e.seq, old.seq);
        } else {
          const row = { ...e }; delete row.seq;
          row.seq = await request(tx.objectStore(stores.events).add(row));
          remap.set(e.seq, row.seq); eventIds.set(key, row); addedEvents.push(row);
        }
      }
      const sourceSeqs = [...remap.keys()].sort((a, b) => a - b);
      // Seq ranges are source-local: keep membership and order, even of an earlier import's list.
      const evidence = (row, from = row.cursor ?? 0) => {
        const source = row.evidenceSeqs ?? sourceSeqs.filter(seq => seq > from && (row.throughSeq == null || seq <= row.throughSeq));
        const evidenceSeqs = [...new Set(source.filter(seq => remap.has(seq)).map(seq => remap.get(seq)))];
        const low = evidenceSeqs.reduce((n, seq) => Math.min(n, seq), Infinity);
        const high = evidenceSeqs.reduce((n, seq) => Math.max(n, seq), 0);
        return { evidenceSeqs, from: low === Infinity ? 0 : low - 1, throughSeq: high };
      };
      for (const name of ["pps", "quarantine", "log"]) {
        const key = r => name === "pps" ? `${r.master_boot_id}:${r.tick}` : name === "quarantine" ? r.line : canonical(portable(r));
        const known = new Map(existing[name].map(r => [key(r), r]));
        for (const r of data[name]) {
          const old = known.get(key(r));
          if (old) {
            if (name !== "log" && !equivalent({ ...old, received_at: 0, at: 0 }, { ...r, received_at: 0, at: 0 })) fail(`Conflicting ${name}. Nothing was imported.`);
            continue;
          }
          const row = { ...r }; delete row.id;
          if (name === "log" && row.throughSeq != null) {
            const refs = evidence(row, 0);
            row.evidenceSeqs = refs.evidenceSeqs;
            row.throughSeq = refs.throughSeq;
          }
          await request(tx.objectStore(stores[name]).add(row)); known.set(key(r), r);
        }
      }
      for (const r of data.runs) {
        if (runsById.has(r.runId) && !runsById.get(r.runId).deleted) {
          const old = runsById.get(r.runId);
          if (old.durable === false) {
            const saved = { ...old, durable: true };
            await request(tx.objectStore(stores.runs).put(saved)); addedRuns.push(saved);
          }
          if (isRecord(r)) skipped++;
          continue;
        }
        // Explicit import may restore a previously deleted record.
        const refs = evidence(r);
        const row = { ...r, durable: true, armed: false, closed: true, imported: true, cursor: refs.from,
          throughSeq: refs.throughSeq, evidenceSeqs: refs.evidenceSeqs,
          nodes: Object.fromEntries(Object.entries(r.nodes).map(([node, v]) => [node, { ...v, cpSeq: v.cpSeq == null ? v.cpSeq : remap.get(v.cpSeq) ?? 0 }])) };
        await request(tx.objectStore(stores.runs).put(row)); addedRuns.push(row);
        if (isRecord(row)) added++;
      }
      await finished;
    } catch (error) {
      try { tx.abort(); } catch { /* already aborted */ }
      await finished.catch(() => {});
      throw error;
    }
    runLog.adopt(addedRuns);
    eventLog.adopt(addedEvents);
    return { added, skipped };
  });
}
