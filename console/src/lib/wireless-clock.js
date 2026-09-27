// The arm boundary must be captured after the request reaches the master.
// A cached heartbeat (or the operator PC's clock) cannot fence queued edges.
function randomHex(bytes) {
  const buf = new Uint8Array(bytes);
  globalThis.crypto.getRandomValues(buf);
  return Array.from(buf, (b) => b.toString(16).padStart(2, "0")).join("");
}

export function createWirelessClock({ send, timeoutMs = 5000 }) {
  const pending = new Map();
  return {
    read() {
      if (pending.size >= 8) return Promise.reject(new Error("Too many pending master clock requests."));
      return new Promise((resolve, reject) => {
        const request_id = randomHex(16);
        const timer = setTimeout(() => {
          pending.delete(request_id);
          reject(new Error("The master did not confirm the start time."));
        }, timeoutMs);
        pending.set(request_id, { resolve, reject, timer });
        try {
          send({ action: "clock", request_id });
        } catch (error) {
          pending.delete(request_id);
          clearTimeout(timer);
          reject(error);
        }
      });
    },
    accept({ request_id, master_tick, master_boot_id }) {
      const request = pending.get(request_id);
      if (!request) return false;
      pending.delete(request_id);
      clearTimeout(request.timer);
      request.resolve({ master_tick, master_boot_id });
      return true;
    },
    // Reject every outstanding request (disconnect, `X clock`).
    close(reason = "Master clock request closed.") {
      for (const request of pending.values()) {
        clearTimeout(request.timer);
        request.reject(new Error(reason));
      }
      pending.clear();
    },
  };
}
