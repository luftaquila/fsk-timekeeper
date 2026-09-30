import { describe, it } from "vitest";
import assert from "node:assert/strict";
import { transaction, add, put, addAll, remove, removeRange, clear, STRICT } from "../src/lib/idb.js";

// fake-indexeddb ignores the durability hint, so check what the wrapper asks for.
function recordingDb() {
  const calls = [];
  const store = {
    add: () => fakeRequest(1),
    put: () => fakeRequest(1),
    delete: () => fakeRequest(undefined),
    clear: () => fakeRequest(undefined),
  };
  function fakeRequest(result) {
    const req = { result };
    setTimeout(() => req.onsuccess?.(), 0);
    return req;
  }
  const db = {
    transaction(stores, mode, options) {
      calls.push({ stores, mode, options });
      const tx = { objectStore: () => store };
      setTimeout(() => tx.oncomplete?.(), 5);
      return tx;
    },
  };
  return { db, calls };
}

describe("idb wrapper", () => {
  it("asks every readwrite transaction for strict durability", async () => {
    const { db, calls } = recordingDb();
    await add(db, "events", {});
    await put(db, "results", {});
    await addAll(db, "events", [{}, {}]);
    await remove(db, "log", 1);
    await removeRange(db, "log", null);
    await clear(db, "pps");
    assert.equal(calls.length, 6);
    for (const call of calls) {
      assert.equal(call.mode, "readwrite");
      assert.deepEqual(call.options, { durability: "strict" });
    }
    transaction(db, "events");
    assert.equal(calls.at(-1).options, undefined);
    assert.deepEqual(STRICT, { durability: "strict" });
  });
});
