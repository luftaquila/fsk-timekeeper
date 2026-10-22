/* Downscaled camera clips (IndexedDB `photos`, keyed "<session>:<capture_seq>"), so History shows
 * a record's crossings without the camera Pi, whose disk keeps the full-resolution clips.
 *   { key, session, seq, meta (the Pi's clip metadata), image: Blob (WebP), savedAt } */
import * as eventLog from "./eventLog";
import { transaction, request, done } from "./idb";

const memory = new Map(); // without IndexedDB, for this page only

export const photoKey = (session, seq) => `${session}:${seq}`;

export async function savePhoto({ session, seq, meta, image }, now = Date.now()) {
  const row = { key: photoKey(session, seq), session: String(session), seq, meta, image, savedAt: now };
  memory.set(row.key, row);
  const db = eventLog.getDb();
  if (!db) return row;
  try {
    const tx = transaction(db, eventLog.STORE_PHOTOS, "readwrite");
    const finished = done(tx);
    tx.objectStore(eventLog.STORE_PHOTOS).put(row);
    await finished;
  } catch (error) {
    eventLog.reportWriteFailure(error);
  }
  return row;
}

export async function getPhoto(key) {
  if (memory.has(key)) return memory.get(key);
  const db = eventLog.getDb();
  if (!db) return null;
  const row = await request(transaction(db, eventLog.STORE_PHOTOS).objectStore(eventLog.STORE_PHOTOS).get(key));
  if (row) memory.set(key, row);
  return row ?? null;
}

export function forgetMemory() {
  memory.clear();
}
