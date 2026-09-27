// Minimal promise wrapper over IndexedDB.

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

export function done(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error || new Error("transaction aborted"));
  });
}

// Resolves after the write transaction commits (durability point).
export async function add(db, store, value) {
  const tx = db.transaction(store, "readwrite");
  const key = await request(tx.objectStore(store).add(value));
  await done(tx);
  return key;
}

export async function put(db, store, value) {
  const tx = db.transaction(store, "readwrite");
  const key = await request(tx.objectStore(store).put(value));
  await done(tx);
  return key;
}

export async function getByIndex(db, store, index, key) {
  return request(db.transaction(store, "readonly").objectStore(store).index(index).get(key));
}

export async function getAll(db, store, range = null, count = undefined) {
  return request(db.transaction(store, "readonly").objectStore(store).getAll(range, count));
}

export async function count(db, store) {
  return request(db.transaction(store, "readonly").objectStore(store).count());
}

export async function remove(db, store, key) {
  const tx = db.transaction(store, "readwrite");
  tx.objectStore(store).delete(key);
  await done(tx);
}

export async function removeRange(db, store, range) {
  const tx = db.transaction(store, "readwrite");
  tx.objectStore(store).delete(range);
  await done(tx);
}

export async function clear(db, store) {
  const tx = db.transaction(store, "readwrite");
  tx.objectStore(store).clear();
  await done(tx);
}
