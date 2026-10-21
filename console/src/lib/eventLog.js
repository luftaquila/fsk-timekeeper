/* Evidence rows: IndexedDB `events` store + in-memory mirror for the engine. An E line's rows
 * commit in one transaction before its ack (the master re-sends until then). Without IndexedDB,
 * or after a write failure, rows stay in memory: stalling the master queue would be worse. */
import { openDb, transaction, request, done, addAll, getAll, count, removeRange } from "./idb";
import { rowKey, lineKey } from "./protocol";
import * as runLog from "./runLog";

export const DB_NAME = "fsk-timekeeper";
export const DB_VERSION = 4;
export const STORE_EVENTS = "events";
export const STORE_PPS = "pps";
export const STORE_LOG = "log";
export const STORE_QUARANTINE = "quarantine";
export const RETENTION = 100000;
const MEMORY_WINDOW = 20000;
const PRUNE_EVERY = 500;
const ROW_INDEX = ["master_boot_id", "node_id", "sensor_boot_id", "kind", "capture_seq", "master_tick"];

let db = null;
let durable = false;
let initError = null;
let rows = []; // sorted by seq
const keyToSeq = new Map(); // rowKey -> seq
const lines = new Set(); // lineKey of every stored or quarantined E line
let lastSeq = 0;
let chain = Promise.resolve();
let insertsSincePrune = 0;
const failureListeners = new Set();

// A database of another version is not migrated: its stores are dropped and recreated.
function upgrade(database) {
  for (const name of [...database.objectStoreNames]) database.deleteObjectStore(name);
  const events = database.createObjectStore(STORE_EVENTS, { keyPath: "seq", autoIncrement: true });
  events.createIndex("node_boot", ["node_id", "master_boot_id"]);
  events.createIndex("row", ROW_INDEX, { unique: true });
  database.createObjectStore(STORE_PPS, { keyPath: ["master_boot_id", "tick"] }).createIndex("received_at", "received_at");
  database.createObjectStore(STORE_LOG, { keyPath: "id", autoIncrement: true }).createIndex("at", "at");
  database.createObjectStore(STORE_QUARANTINE, { keyPath: "id", autoIncrement: true }).createIndex("line", "line", { unique: true });
  database.createObjectStore(runLog.STORE_RUNS, { keyPath: "runId" });
  database.createObjectStore(runLog.STORE_META, { keyPath: "key" });
}

export function getDb() {
  return db;
}

export function isDurable() {
  return durable;
}

export function getInitError() {
  return initError;
}

// cb(error) after an IndexedDB write failed (the log continues in memory).
export function onWriteFailure(cb) {
  failureListeners.add(cb);
  return () => failureListeners.delete(cb);
}

// Any module whose IndexedDB write failed reports it here; storage stays memory-only after.
export function reportWriteFailure(error) {
  durable = false;
  initError = error;
  try {
    db?.close();
  } catch {
    /* ignore */
  }
  db = null;
  for (const cb of failureListeners) {
    try {
      cb(error);
    } catch {
      /* listener errors must not break ingest */
    }
  }
}

function remember(row) {
  if (row.kind && row.kind !== "quarantine") keyToSeq.set(rowKey(row), row.seq);
  if (row.hseq != null) lines.add(lineKey(row.master_boot_id, row.hseq));
}

// The open run's evidence (rows after its cursor) and the last MEMORY_WINDOW rows are loaded.
export async function init() {
  try {
    db = await openDb(DB_NAME, DB_VERSION, upgrade);
    durable = true;
    const activeRun = await runLog.init(db);
    const minSeq = activeRun && !activeRun.closed ? activeRun.cursor : Infinity;
    const tail = await getAllDesc(STORE_EVENTS, MEMORY_WINDOW);
    let loaded = tail;
    if (Number.isFinite(minSeq) && tail.length && tail[tail.length - 1].seq > minSeq + 1) {
      loaded = await getAll(db, STORE_EVENTS, IDBKeyRange.lowerBound(minSeq, true));
    }
    rows = loaded.sort((a, b) => a.seq - b.seq);
    for (const row of rows) remember(row);
    for (const q of await getAll(db, STORE_QUARANTINE)) lines.add(q.line);
    lastSeq = rows.length ? rows[rows.length - 1].seq : await lastKey(STORE_EVENTS);
  } catch (error) {
    db = null;
    durable = false;
    initError = error;
  }
  return { durable, error: initError };
}

async function getAllDesc(store, limit) {
  const out = [];
  await new Promise((resolve, reject) => {
    const req = transaction(db, store).objectStore(store).openCursor(null, "prev");
    req.onsuccess = () => {
      const cursor = req.result;
      if (!cursor || out.length >= limit) return resolve();
      out.push(cursor.value);
      cursor.continue();
    };
    req.onerror = () => reject(req.error);
  });
  return out;
}

async function lastKey(store) {
  const req = transaction(db, store).objectStore(store).openKeyCursor(null, "prev");
  const cursor = await request(req);
  return cursor ? cursor.key : 0;
}

export function since(seq) {
  if (!Number.isFinite(seq)) return rows.slice();
  let lo = 0;
  let hi = rows.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (rows[mid].seq > seq) hi = mid;
    else lo = mid + 1;
  }
  return rows.slice(lo);
}

export function getLastSeq() {
  return lastSeq;
}

export async function flush() { await chain; }

// Call only after live ingestion and run writes have drained.
export function clearAllData() {
  return serialize(async () => {
    // A write failure can close our connection while leaving old data on disk.
    const storage = db || (typeof indexedDB !== "undefined" ? await openDb(DB_NAME, DB_VERSION, upgrade) : null);
    try {
      if (storage) {
        const tx = transaction(storage, Array.from(storage.objectStoreNames), "readwrite");
        const finished = done(tx);
        for (const name of storage.objectStoreNames) tx.objectStore(name).clear();
        await finished;
      }
    } finally {
      if (storage && storage !== db) storage.close();
    }
    rows = [];
    keyToSeq.clear();
    lines.clear();
    lastSeq = 0;
    insertsSincePrune = 0;
    protectSeq = Infinity;
    runLog.reset();
  });
}

// Import adds archived evidence without sending it through the live run engine.
export function adopt(imported) {
  const known = new Set(rows.map(r => r.seq));
  for (const row of imported) if (!known.has(row.seq)) { append(row); known.add(row.seq); }
  rows.sort((a, b) => a.seq - b.seq);
}

// Latest checkpoint row of a node under a master session at or before maxTick.
export function latestCheckpoint(node, masterBootId, maxTick) {
  const limit = BigInt(maxTick);
  for (let i = rows.length - 1; i >= 0; i--) {
    const row = rows[i];
    if (row.kind !== "checkpoint" || row.node_id !== node || row.master_boot_id !== masterBootId) continue;
    if (BigInt(row.master_tick) <= limit) return row;
  }
  return null;
}

function append(row) {
  if (row.seq == null) row.seq = lastSeq + 1;
  lastSeq = Math.max(lastSeq, row.seq);
  rows.push(row);
  remember(row);
  if (++insertsSincePrune >= PRUNE_EVERY) {
    insertsSincePrune = 0;
    pruneMemory();
  }
}

function serialize(task) {
  const p = chain.then(task);
  chain = p.catch(() => {});
  return p;
}

// Store the rows of one valid E line (one transaction). -> { duplicate, rows }
export function ingestLine({ rows: incoming, masterBootId, hseq }, receivedAt = Date.now()) {
  return serialize(async () => {
    const key = lineKey(masterBootId, hseq);
    if (lines.has(key) || incoming.every((row) => keyToSeq.has(rowKey(row)))) return { duplicate: true, rows: [] };
    const stored = incoming.map((row) => ({ ...row, received_at: receivedAt }));
    if (db) {
      try {
        const keys = await addAll(db, STORE_EVENTS, stored);
        stored.forEach((row, i) => (row.seq = keys[i]));
      } catch (error) {
        if (error?.name === "ConstraintError") {
          // Committed before this page load and no longer in memory.
          lines.add(key);
          return { duplicate: true, rows: [] };
        }
        stored.forEach((row) => delete row.seq);
        reportWriteFailure(error);
      }
    }
    for (const row of stored) append(row);
    return { duplicate: false, rows: stored };
  });
}

// Quarantine a crc-valid, contract-breaking E line: raw text + event-log marker in one transaction.
export function ingestQuarantine({ raw, reason, node, hseq, masterBootId }, receivedAt = Date.now()) {
  return serialize(async () => {
    const key = lineKey(masterBootId, hseq);
    if (lines.has(key)) return { duplicate: true, marker: null };
    const marker = { kind: "quarantine", node_id: node ?? "*", master_boot_id: masterBootId ?? null, hseq, raw, reason, received_at: receivedAt };
    const record = { line: key, masterBootId: masterBootId ?? null, hseq, raw, reason, node_id: node ?? null, at: receivedAt };
    if (db) {
      try {
        const tx = transaction(db, [STORE_EVENTS, STORE_QUARANTINE], "readwrite");
        const finished = done(tx);
        const seqReq = request(tx.objectStore(STORE_EVENTS).add(marker));
        const qReq = request(tx.objectStore(STORE_QUARANTINE).add(record));
        const [seq] = await Promise.all([seqReq, qReq]).catch(async (error) => {
          await finished.catch(() => {});
          throw error;
        });
        await finished;
        marker.seq = seq;
      } catch (error) {
        if (error?.name === "ConstraintError") {
          lines.add(key);
          return { duplicate: true, marker: null };
        }
        delete marker.seq;
        reportWriteFailure(error);
      }
    }
    lines.add(key);
    append(marker);
    return { duplicate: false, marker, record };
  });
}

export async function listQuarantine() {
  if (!db) return rows.filter((row) => row.kind === "quarantine").map((row) => ({ line: lineKey(row.master_boot_id, row.hseq), masterBootId: row.master_boot_id, hseq: row.hseq, raw: row.raw, reason: row.reason, node_id: row.node_id === "*" ? null : row.node_id, at: row.received_at }));
  return getAll(db, STORE_QUARANTINE);
}

let protectSeq = Infinity;
// Rows with seq > protectSeq are kept in memory regardless of the window (open runs).
export function protect(seq) {
  protectSeq = Number.isFinite(seq) ? seq : Infinity;
}

function pruneMemory() {
  const keepFrom = Math.min(protectSeq, lastSeq - MEMORY_WINDOW);
  if (rows.length <= MEMORY_WINDOW || rows[0].seq >= keepFrom) return;
  const drop = since(keepFrom).length;
  const removed = rows.splice(0, rows.length - drop);
  for (const row of removed) {
    if (row.kind && row.kind !== "quarantine") keyToSeq.delete(rowKey(row));
    // The master only re-sends its queue head, so lines this old never come again.
    if (row.hseq != null && row.kind !== "quarantine") lines.delete(lineKey(row.master_boot_id, row.hseq));
  }
}

// Durable retention: keep the newest RETENTION rows, never below protectSeq.
export async function pruneStore() {
  if (!db) return 0;
  const total = await count(db, STORE_EVENTS);
  if (total <= RETENTION) return 0;
  const cutoff = Math.min(protectSeq, lastSeq - RETENTION);
  if (!(cutoff > 0)) return 0;
  await removeRange(db, STORE_EVENTS, IDBKeyRange.upperBound(cutoff, true));
  return total - RETENTION;
}

export function close() {
  try {
    db?.close();
  } catch {
    /* ignore */
  }
  db = null;
}

// Test hook: close the database and drop in-memory state.
export function _reset() {
  runLog.reset();
  close();
  durable = false;
  initError = null;
  rows = [];
  keyToSeq.clear();
  lines.clear();
  lastSeq = 0;
  chain = Promise.resolve();
  insertsSincePrune = 0;
  protectSeq = Infinity;
  failureListeners.clear();
}
