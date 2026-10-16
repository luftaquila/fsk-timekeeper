import * as eventLog from "./eventLog";
import { getAll, transaction, done } from "./idb";
import { enqueue, flush } from "./storageQueue";

export const STORE_RUNS = "runs";
export const STORE_META = "meta";
let records = new Map();
let activeId = null;
let initialized = false;
let generation = 0;
export const clone = value => JSON.parse(JSON.stringify(value, (_k, v) => typeof v === "bigint" ? String(v) : v));
export const epoch = () => generation;
export const hasActiveState = () => initialized;
export const active = () => records.get(activeId) || null;
export const all = () => [...records.values()].filter(r => !r.deleted).map(clone);
export const get = id => records.get(id);
export const isDeleted = id => records.get(id)?.deleted === true;
export { flush };

export async function init(db) {
  records = new Map((await getAll(db, STORE_RUNS)).map(r => [r.runId, r]));
  const state = (await getAll(db, STORE_META)).find(r => r.key === "activeRun");
  initialized = !!state;
  activeId = state?.runId ?? null;
  return active();
}

// Update the mirror immediately; commit snapshots in the same order as history.
export function save(run, { select = false } = {}) {
  if (run && isDeleted(run.runId)) return Promise.resolve(false);
  const snapshot = run ? clone(run) : null;
  if (snapshot) records.set(snapshot.runId, snapshot);
  if (select) { activeId = snapshot?.runId ?? null; initialized = true; }
  const selected = activeId;
  const version = generation;
  return enqueue(async () => {
    if (version !== generation) return false;
    if (snapshot && isDeleted(snapshot.runId)) return false;
    const db = eventLog.getDb();
    if (!db) {
      if (snapshot && !isDeleted(snapshot.runId)) records.get(snapshot.runId).durable = false;
      return false;
    }
    try {
      const tx = transaction(db, [STORE_RUNS, STORE_META], "readwrite");
      const finished = done(tx);
      if (snapshot) tx.objectStore(STORE_RUNS).put(snapshot);
      if (select) tx.objectStore(STORE_META).put({ key: "activeRun", runId: selected });
      await finished;
      return true;
    } catch (error) {
      if (snapshot && !isDeleted(snapshot.runId)) records.get(snapshot.runId).durable = false;
      eventLog.reportWriteFailure(error);
      return false;
    }
  });
}

// Called inside the shared mutation queue; the caller owns the DB transaction.
export function tombstone(id) { records.set(id, { runId: id, deleted: true }); }
export function forgetTombstone(id) { if (isDeleted(id)) records.delete(id); }
export function adopt(runs) { for (const run of runs) records.set(run.runId, clone(run)); }
export function reset() { records = new Map(); activeId = null; initialized = false; generation++; }
