// Device log (IndexedDB `log`): { id, at, kind: "X"|"boot"|"console"|"radio"|"state", code, text,
// devid, masterBootId }; kept 30 days, trimmed to LOG_MAX_ENTRIES ("state" stays until expiry).
import { add, getAll, clear, removeByIndex, count, transaction, done } from "./idb";
import * as eventLog from "./eventLog";
import { LOG_RETENTION_MS, LOG_MAX_ENTRIES } from "./constants";
import { logCode } from "./deviceLogFormat";

let memory = [];
let memoryId = 0;

export function resetMemory() { memory = []; memoryId = 0; }

export async function addEntry(entry) {
  const row = { ...entry, code: logCode(entry.code) };
  const db = eventLog.getDb();
  if (db) {
    try {
      row.id = await add(db, eventLog.STORE_LOG, row);
      return row;
    } catch (error) {
      eventLog.reportWriteFailure(error);
    }
  }
  row.id = -++memoryId;
  memory.push(row);
  if (memory.length > LOG_MAX_ENTRIES) memory.splice(0, memory.length - LOG_MAX_ENTRIES);
  return row;
}

// Newest first.
export async function listEntries() {
  const db = eventLog.getDb();
  let stored = [];
  if (db) {
    try {
      stored = await getAll(db, eventLog.STORE_LOG);
    } catch {
      stored = [];
    }
  }
  return [...stored, ...memory].map((e) => ({ ...e, code: logCode(e.code) })).sort((a, b) => b.at - a.at || b.id - a.id);
}

export async function clearEntries() {
  memory = [];
  const db = eventLog.getDb();
  if (db) await clear(db, eventLog.STORE_LOG);
}

export async function pruneEntries(now = Date.now()) {
  const db = eventLog.getDb();
  if (!db) return;
  try {
    await removeByIndex(db, eventLog.STORE_LOG, "at", IDBKeyRange.upperBound(now - LOG_RETENTION_MS));
    const total = await count(db, eventLog.STORE_LOG);
    if (total <= LOG_MAX_ENTRIES) return;
    // Oldest first by primary key: delete the surplus.
    const tx = transaction(db, eventLog.STORE_LOG, "readwrite");
    const finished = done(tx);
    let surplus = total - LOG_MAX_ENTRIES;
    const req = tx.objectStore(eventLog.STORE_LOG).openCursor();
    req.onsuccess = () => {
      const cursor = req.result;
      if (!cursor || surplus <= 0) return;
      // Keep the Clear watermark for its 30 days, or replayed checkpoints resurrect cleared logs.
      if (cursor.value.kind !== "state") {
        tx.objectStore(eventLog.STORE_LOG).delete(cursor.primaryKey);
        surplus--;
      }
      cursor.continue();
    };
    await finished;
  } catch {
    /* retention is best effort */
  }
}
