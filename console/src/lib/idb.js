// Minimal promise wrapper over IndexedDB. Every readwrite transaction asks for strict
// durability: `complete` then means the write reached the disk, not just the OS buffers.

export const STRICT = Object.freeze({ durability: "strict" });

export function request(req) {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

export function openDb(name, version, upgrade) {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") {
      reject(new Error("IndexedDB is not available"));
      return;
    }
    const req = indexedDB.open(name, version);
    req.onupgradeneeded = (event) => upgrade(req.result, event.oldVersion, req.transaction);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
    req.onblocked = () => reject(new Error("IndexedDB open blocked"));
  });
}

// The one place transactions are opened.
export function transaction(db, stores, mode = "readonly") {
  return mode === "readwrite" ? db.transaction(stores, mode, STRICT) : db.transaction(stores, mode);
}

export function done(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error || new Error("transaction aborted"));
  });
}

// Resolves after the write transaction commits (durability point).
export async function add(db, store, value) {
  const tx = transaction(db, store, "readwrite");
  const key = await request(tx.objectStore(store).add(value));
  await done(tx);
  return key;
}

export async function put(db, store, value) {
  const tx = transaction(db, store, "readwrite");
  const key = await request(tx.objectStore(store).put(value));
  await done(tx);
  return key;
}

// Several adds in one transaction: all commit or none (a constraint error aborts the lot).
export async function addAll(db, store, values) {
  const tx = transaction(db, store, "readwrite");
  const finished = done(tx);
  const os = tx.objectStore(store);
  const settled = await Promise.allSettled(values.map((value) => request(os.add(value))));
  try {
    await finished;
  } catch (error) {
    throw settled.find((s) => s.status === "rejected")?.reason || error;
  }
  return settled.map((s) => s.value);
}

export async function getByIndex(db, store, index, key) {
  return request(transaction(db, store).objectStore(store).index(index).get(key));
}

export async function getAll(db, store, range = null, count = undefined) {
  return request(transaction(db, store).objectStore(store).getAll(range, count));
}

export async function getAllByIndex(db, store, index, range = null) {
  return request(transaction(db, store).objectStore(store).index(index).getAll(range));
}

export async function count(db, store) {
  return request(transaction(db, store).objectStore(store).count());
}

export async function remove(db, store, key) {
  const tx = transaction(db, store, "readwrite");
  tx.objectStore(store).delete(key);
  await done(tx);
}

export async function removeRange(db, store, range) {
  const tx = transaction(db, store, "readwrite");
  tx.objectStore(store).delete(range);
  await done(tx);
}

// Delete every record whose index key falls in range.
export async function removeByIndex(db, store, index, range) {
  const tx = transaction(db, store, "readwrite");
  const finished = done(tx);
  const req = tx.objectStore(store).index(index).openKeyCursor(range);
  req.onsuccess = () => {
    const cursor = req.result;
    if (!cursor) return;
    tx.objectStore(store).delete(cursor.primaryKey);
    cursor.continue();
  };
  await finished;
}

export async function clear(db, store) {
  const tx = transaction(db, store, "readwrite");
  tx.objectStore(store).clear();
  await done(tx);
}
