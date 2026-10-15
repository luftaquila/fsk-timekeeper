import { afterEach, describe, expect, it, vi } from "vitest";
import { createSerialTransport } from "../src/transport/serial";

function serialHarness() {
  const events = [];
  let finishRead;
  const reader = {
    read: vi.fn(() => new Promise((resolve) => { finishRead = resolve; })),
    cancel: vi.fn(async () => {
      events.push("cancel");
      finishRead?.({ done: true });
    }),
    releaseLock: vi.fn(),
  };
  const writer = {
    write: vi.fn(async () => { events.push("write"); }),
    releaseLock: vi.fn(),
  };
  const port = {
    open: vi.fn(async () => { events.push("open"); }),
    close: vi.fn(async () => { events.push("close"); }),
    setSignals: vi.fn(async ({ dataTerminalReady }) => {
      events.push(dataTerminalReady ? "dtr-on" : "dtr-off");
    }),
    readable: { getReader: () => { events.push("reader"); return reader; } },
    writable: { getWriter: () => writer },
    getInfo: () => ({ usbVendorId: 0x239a }),
  };
  vi.stubGlobal("navigator", {
    serial: { requestPort: vi.fn(async () => port), addEventListener: vi.fn() },
  });
  const onDisconnect = vi.fn();
  const transport = createSerialTransport({ onLine: vi.fn(), onDisconnect });
  return { events, port, reader, writer, transport, onDisconnect };
}

afterEach(() => { vi.unstubAllGlobals(); });

describe("serial console session", () => {
  it("starts reading before enabling reports, then stops reports before closing", async () => {
    const h = serialHarness();
    await h.transport.open();
    expect(h.events).toEqual(["open", "reader", "dtr-on"]);
    expect(await h.transport.write("?ID")).toBe(true);
    await h.transport.close();
    expect(h.events).toEqual(["open", "reader", "dtr-on", "write", "dtr-off", "cancel", "close"]);
    expect(h.transport.connected).toBe(false);
    expect(h.onDisconnect).not.toHaveBeenCalled();
    await h.transport.open();
    expect(h.events.slice(-3)).toEqual(["open", "reader", "dtr-on"]);
    await h.transport.close();
  });

  it("releases the reader and port when DTR cannot be asserted", async () => {
    const h = serialHarness();
    h.port.setSignals.mockRejectedValueOnce(new Error("DTR failed"));
    await expect(h.transport.open()).rejects.toThrow("DTR failed");
    expect(h.reader.cancel).toHaveBeenCalledOnce();
    expect(h.port.close).toHaveBeenCalledOnce();
    expect(h.transport.connected).toBe(false);
    expect(h.onDisconnect).not.toHaveBeenCalled();
  });

  it("finishes an in-flight write before lowering DTR and rejects new writes during close", async () => {
    const h = serialHarness();
    await h.transport.open();
    let finishWrite;
    h.writer.write.mockImplementationOnce(() => new Promise((resolve) => { finishWrite = resolve; }));
    const writing = h.transport.write("PING");
    await Promise.resolve();
    expect(finishWrite).toBeTypeOf("function");
    const closing = h.transport.close();
    const lateWrite = h.transport.write("?STATUS");
    expect(h.port.setSignals).toHaveBeenCalledTimes(1);
    finishWrite();
    expect(await writing).toBe(true);
    expect(await lateWrite).toBe(false);
    await closing;
    expect(h.writer.releaseLock).toHaveBeenCalledOnce();
    expect(h.events.slice(-3)).toEqual(["dtr-off", "cancel", "close"]);
  });
});
