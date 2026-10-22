/* WebSocket transport to the camera Pi's master bridge (wss://<host>/api/master). The Pi owns the
 * master's USB port and relays its lines unchanged, one text frame per line both ways; interface
 * shared with transport/serial.js. The bridge serves one console at a time (close code 4409). */
export const PI_BUSY_CLOSE = 4409;

export function createPiTransport({ onLine, onDisconnect }, { host, WebSocketImpl = globalThis.WebSocket } = {}) {
  let ws = null;
  let intentionalClose = false;

  function open() {
    if (!WebSocketImpl) return Promise.reject(new Error("WebSocket is not available"));
    intentionalClose = false;
    const socket = new WebSocketImpl(`wss://${host}/api/master`);
    return new Promise((resolve, reject) => {
      socket.onopen = () => {
        ws = socket;
        socket.onclose = () => {
          if (ws !== socket) return;
          ws = null;
          if (!intentionalClose) onDisconnect?.();
        };
        resolve({ host });
      };
      socket.onerror = () => {};
      socket.onclose = (event) => {
        reject(new Error(event?.code === PI_BUSY_CLOSE ? "Another console is using this camera" : `Cannot reach ${host}`));
      };
      socket.onmessage = (event) => {
        for (const line of String(event.data).split("\n")) if (line) onLine?.(line);
      };
    });
  }

  async function write(line) {
    if (intentionalClose || ws?.readyState !== 1) return false;
    try {
      ws.send(line);
      return true;
    } catch {
      return false;
    }
  }

  async function close() {
    intentionalClose = true;
    const socket = ws;
    ws = null;
    try {
      socket?.close();
    } catch {
      /* already closed */
    }
  }

  async function enterBootloader() {
    throw new Error("Update the camera master over USB from this computer.");
  }

  return {
    kind: "pi",
    host,
    open,
    close,
    write,
    enterBootloader,
    get connected() {
      return !!ws;
    },
  };
}
