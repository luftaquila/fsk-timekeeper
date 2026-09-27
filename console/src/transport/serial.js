/* Web Serial transport for the FSK-WL master (CDC-ACM, newline-delimited ASCII).
 *
 * Interface shared with transport/fake.js:
 *   open() -> Promise<info>   close() -> Promise   write(line) -> Promise<boolean>
 *   enterBootloader() -> Promise      kind: "serial"
 * Callbacks: onLine(line), onDisconnect()
 */
import { USB_VID, USB_PID, SERIAL_BAUD } from "../lib/constants";

export function isSerialSupported() {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

export function createSerialTransport({ onLine, onDisconnect }) {
  let port = null;
  let reader = null;
  let intentionalClose = false;
  let writeChain = Promise.resolve(true);
  const filters = [{ usbVendorId: USB_VID, usbProductId: USB_PID }];

  if (isSerialSupported()) {
    // The read loop may not notice a yanked cable immediately; the OS-level event does.
    navigator.serial.addEventListener("disconnect", (e) => {
      if (port && e.target === port) {
        const wasIntentional = intentionalClose;
        cleanup();
        if (!wasIntentional) onDisconnect?.();
      }
    });
  }

  function cleanup() {
    try {
      reader?.releaseLock();
    } catch {
      /* ignore */
    }
    reader = null;
    port = null;
  }

  async function open() {
    if (!isSerialSupported()) throw new Error("Web Serial is not supported by this browser. Use Chrome or Edge.");
    const candidate = await navigator.serial.requestPort({ filters });
    await candidate.open({ baudRate: SERIAL_BAUD });
    port = candidate;
    intentionalClose = false;
    readLoop();
    return port.getInfo?.() || {};
  }

  async function readLoop() {
    const current = port;
    const decoder = new TextDecoder();
    let buffer = "";
    try {
      reader = current.readable.getReader();
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        buffer += decoder.decode(value, { stream: true });
        let idx;
        while ((idx = buffer.indexOf("\n")) > -1) {
          const line = buffer.slice(0, idx);
          buffer = buffer.slice(idx + 1);
          onLine?.(line);
        }
      }
    } catch {
      /* device removed / reader cancelled — handled below */
    } finally {
      try {
        reader?.releaseLock();
      } catch {
        /* ignore */
      }
      reader = null;
    }
    if (port === current && !intentionalClose) {
      port = null;
      onDisconnect?.();
    }
  }

  // Serialized writes: concurrent callers never interleave bytes.
  function write(line) {
    const task = async () => {
      if (!port?.writable) return false;
      const writer = port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(line + "\n"));
        return true;
      } catch {
        return false;
      } finally {
        writer.releaseLock();
      }
    };
    writeChain = writeChain.then(task, task);
    return writeChain;
  }

  async function close() {
    intentionalClose = true;
    const current = port;
    try {
      await reader?.cancel();
    } catch {
      /* ignore */
    }
    try {
      await current?.close();
    } catch {
      /* ignore */
    }
    cleanup();
  }

  // Adafruit nRF52 bootloader touch: open at 1200 baud, drop DTR, close.
  // The app sets GPREGRET=0x57 and resets; the board re-enumerates as the bootloader.
  async function enterBootloader() {
    const current = port;
    if (!current) throw new Error("Not connected.");
    await close();
    await current.open({ baudRate: 1200 });
    try {
      await current.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch {
      /* some drivers reject setSignals; closing the port drops DTR anyway */
    }
    await new Promise((r) => setTimeout(r, 100));
    try {
      await current.close();
    } catch {
      /* the device usually vanishes before close resolves */
    }
  }

  return {
    kind: "serial",
    open,
    close,
    write,
    enterBootloader,
    get connected() {
      return !!port;
    },
  };
}
