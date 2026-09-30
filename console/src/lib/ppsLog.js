// Qualified PPS edges per master boot (IndexedDB `pps`), the input of the PPS timeline.
import { put, getAll, removeByIndex } from "./idb";
import * as eventLog from "./eventLog";
import { PPS_RETENTION_MS } from "./constants";

export async function saveEdge(edge) {
  const db = eventLog.getDb();
  if (!db) return false;
  try {
    await put(db, eventLog.STORE_PPS, edge);
    return true;
  } catch (error) {
    eventLog.reportWriteFailure(error);
    return false;
  }
}

// Edges of one master boot, sorted by tick.
export async function loadEdges(masterBootId) {
  const db = eventLog.getDb();
  if (!db || masterBootId == null) return [];
  try {
    const edges = await getAll(db, eventLog.STORE_PPS, IDBKeyRange.bound([masterBootId, ""], [masterBootId, "￿"]));
    return edges.sort((a, b) => (BigInt(a.tick) < BigInt(b.tick) ? -1 : 1));
  } catch {
    return [];
  }
}

export async function pruneEdges(now = Date.now()) {
  const db = eventLog.getDb();
  if (!db) return;
  try {
    await removeByIndex(db, eventLog.STORE_PPS, "received_at", IDBKeyRange.upperBound(now - PPS_RETENTION_MS));
  } catch {
    /* retention is best effort */
  }
}
