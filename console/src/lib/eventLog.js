/* Durable log of raw `E` rows (IndexedDB) with an in-memory mirror the engine reads.
 *
 * The master keeps each event in a 16-slot RAM queue and re-sends the head every
 * 100 ms until the host commits it with `C`. `ingest()` resolves once the row is
 * committed to IndexedDB (or, if IndexedDB is unavailable, once it is in memory —
 * a host that never acks stalls the queue, which is worse than losing durability).
 */
import { openDb, add, getAll, getByIndex, count, removeRange, request } from "./idb";
import { eventKey } from "./protocol";
import { CAPTURE_CHECKPOINT } from "./capture-integrity";

export const DB_NAME = "fsk-timekeeper";
export const DB_VERSION = 1;
export const STORE_EVENTS = "events";
export const STORE_RESULTS = "results";
export const RETENTION = 100000;
const MEMORY_WINDOW = 20000;
const PRUNE_EVERY = 500;

let db = null;
let durable = false;
let initError = null;
let rows = []; // sorted by seq
const keyToSeq = new Map();
let lastSeq = 0;
let chain = Promise.resolve();
let insertsSincePrune = 0;

function upgrade(database, oldVersion) {
  if (oldVersion < 1) {
    const events = database.createObjectStore(STORE_EVENTS, { keyPath: "seq", autoIncrement: true });
    events.createIndex("key", ["node_id", "ev_seq", "master_tick", "master_boot_id", "sensor_boot_id"], { unique: true });
    events.createIndex("node_boot", ["node_id", "master_boot_id"]);
    const results = database.createObjectStore(STORE_RESULTS, { keyPath: "id", autoIncrement: true });
    results.createIndex("runId", "runId", { unique: true });
    results.createIndex("mode", "mode");
    results.createIndex("createdAt", "createdAt");
  }
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

// minSeq: rows with seq > minSeq must be in memory (open-run cursors); the last
// MEMORY_WINDOW rows are always loaded.
export async function init({ minSeq = Infinity } = {}) {
  try {
    db = await openDb(DB_NAME, DB_VERSION, upgrade);
    durable = true;
    const tail = await getAllDesc(STORE_EVENTS, MEMORY_WINDOW);
    let loaded = tail;
    if (Number.isFinite(minSeq) && tail.length && tail[tail.length - 1].seq > minSeq + 1) {
      loaded = await getAll(db, STORE_EVENTS, IDBKeyRange.lowerBound(minSeq, true));
    }
    rows = loaded.sort((a, b) => a.seq - b.seq);
    for (const row of rows) keyToSeq.set(eventKey(row), row.seq);
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
    const req = db.transaction(store, "readonly").objectStore(store).openCursor(null, "prev");
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
  const req = db.transaction(store, "readonly").objectStore(store).openKeyCursor(null, "prev");
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

// Latest checkpoint of a node under a master session at or before maxTick.
export function latestCheckpoint(node, masterBootId, maxTick) {
  const limit = BigInt(maxTick);
  for (let i = rows.length - 1; i >= 0; i--) {
    const row = rows[i];
    if (row.node_id !== node || row.master_boot_id !== masterBootId || !(row.flags & CAPTURE_CHECKPOINT)) continue;
    if (BigInt(row.master_tick) <= limit) return row;
  }
  return null;
}

// Serialized: seq assignment, IDB commit and memory append happen in arrival order.
export function ingest(row, receivedAt = Date.now()) {
  const p = chain.then(async () => {
    const key = eventKey(row);
    const known = keyToSeq.get(key);
    if (known != null) return { seq: known, duplicate: true, row: rows.find((r) => r.seq === known) || row };
    const stored = { ...row, received_at: receivedAt };
    if (db) {
      try {
        stored.seq = await add(db, STORE_EVENTS, stored);
      } catch (error) {
        if (error?.name === "ConstraintError") {
          // Committed in an earlier session and not loaded into memory.
          const existing = await getByIndex(db, STORE_EVENTS, "key", [row.node_id, row.ev_seq, row.master_tick, row.master_boot_id, row.sensor_boot_id]);
          if (existing) {
            keyToSeq.set(key, existing.seq);
            return { seq: existing.seq, duplicate: true, row: existing };
          }
        }
        durable = false;
        initError = error;
        db = null;
      }
    }
    if (stored.seq == null) stored.seq = lastSeq + 1;
    lastSeq = Math.max(lastSeq, stored.seq);
    rows.push(stored);
    keyToSeq.set(key, stored.seq);
    if (++insertsSincePrune >= PRUNE_EVERY) {
      insertsSincePrune = 0;
      pruneMemory();
    }
    return { seq: stored.seq, duplicate: false, row: stored };
  });
  chain = p.catch(() => {});
  return p;
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
  for (const row of removed) keyToSeq.delete(eventKey(row));
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

export async function rowsBetween(fromSeqExclusive, toSeqInclusive) {
  if (db) return getAll(db, STORE_EVENTS, IDBKeyRange.bound(fromSeqExclusive, toSeqInclusive, true, false));
  return rows.filter((r) => r.seq > fromSeqExclusive && r.seq <= toSeqInclusive);
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
  close();
  durable = false;
  initError = null;
  rows = [];
  keyToSeq.clear();
  lastSeq = 0;
  chain = Promise.resolve();
  insertsSincePrune = 0;
  protectSeq = Infinity;
}
