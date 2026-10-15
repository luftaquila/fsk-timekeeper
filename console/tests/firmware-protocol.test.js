import { describe, it, expect } from "vitest";
import { packet, words, crc16, ackNumber, slipDecoder, flashFirmware } from "../src/firmware/protocol";

const ack = sequence => new Uint8Array([0xc0, (sequence & 7) << 3, 0, 0, (-((sequence & 7) << 3)) & 255, 0xc0]);
function bootloader({ silent = false, corrupt = false, wrong = false, disconnect = false, silentFrom = 0, partial = false } = {}) {
  let source;
  const received = [];
  const port = {
    received, closed: false, signals: [],
    async open(options) {
      expect(options.baudRate).toBe(115200);
      this.readable = new ReadableStream({ start(c) { source = c; } });
      const decode = slipDecoder(frame => {
        received.push(frame);
        if (disconnect) { source.close(); return; }
        if (silent || (silentFrom && received.length >= silentFrom)) return;
        if (partial) { source.enqueue(new Uint8Array([0xc0, 0x10])); return; }
        const response = ack((frame[0] & 7) + (wrong ? 0 : 1));
        if (corrupt) response[4] ^= 1;
        // Deliver a prior ACK before the correct ACK, then split the latter.
        if (!wrong) source.enqueue(ack(frame[0] & 7));
        source.enqueue(response.slice(0, 2)); source.enqueue(response.slice(2));
      });
      this.writable = new WritableStream({ write(chunk) { decode(chunk); } });
    },
    async setSignals(value) { this.signals.push(value); },
    async close() { this.closed = true; },
  };
  return port;
}

describe("Adafruit legacy DFU", () => {
  it("uses the CRC16-CCITT check vector", () => {
    expect(crc16(new TextEncoder().encode("123456789"))).toBe(0x29b1);
  });
  it("matches an independently generated adafruit-nrfutil START packet", () => {
    // Generated with HciPacket(int32_to_bytes(3/4/0/0/1024)), sequence starts at 1.
    expect(Buffer.from(packet(words(3, 4, 0, 0, 1024), 1)).toString("hex")).toBe("c0d14e01e00300000004000000000000000000000000040000ab73c0");
  });
  it("validates fragmented SLIP frames and rejects malformed ACKs", () => {
    const frames = []; const decode = slipDecoder(f => frames.push(f));
    for (const byte of ack(2)) decode(new Uint8Array([byte]));
    expect(ackNumber(frames[0])).toBe(2);
    expect(() => ackNumber(new Uint8Array([16, 0, 0, 0]))).toThrow("header");
    expect(() => slipDecoder(() => {})(new Uint8Array([192, 219, 1]))).toThrow("escape");
  });
  it("transmits START, INIT, DATA and STOP, rolls sequence and cleans up", async () => {
    const p = bootloader(), updates = [], delays = [];
    await flashFirmware(p, { binary: new Uint8Array(5000), init: new Uint8Array(14) }, { delay: async ms => { delays.push([ms, p.signals.length, p.received.length]); }, onProgress: (...args) => updates.push(args) });
    expect(delays.slice(0, 3)).toEqual([[100, 0, 0], [50, 1, 0], [100, 2, 0]]);
    expect(p.received.map(f => new DataView(f.buffer).getUint32(4, true))).toEqual([3, 1, ...Array(10).fill(4), 5]);
    expect(p.received.map(f => f[0] & 7)).toEqual([1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5]);
    for (const f of p.received) expect(crc16(f.subarray(0, -2))).toBe(f.at(-2) | (f.at(-1) << 8));
    expect(updates.at(-1)).toEqual(["restarting", 100]);
    expect(p.closed).toBe(true);
    expect(p.readable.locked || p.writable.locked).toBe(false);
  });
  it("does not close a port when opening it was denied", async () => {
    let closed = false;
    const port = { open: async () => { throw new Error("Port busy"); }, close: async () => { closed = true; } };
    await expect(flashFirmware(port, {})).rejects.toThrow("Port busy");
    expect(closed).toBe(false);
  });
  for (const [name, options, error] of [["missing ACK", { silent: true }, /during START. No serial response/], ["wrong ACK", { wrong: true }, /during START. Expected ACK 2, received 1/],
    ["partial ACK", { partial: true }, /Received 2 bytes without a complete ACK/],
    ["corrupt ACK", { corrupt: true }, /header/], ["cable removed", { disconnect: true }, /disconnected/]]) {
    it(`fails closed on ${name} and releases the port`, async () => {
      const p = bootloader(options);
      await expect(flashFirmware(p, { binary: new Uint8Array(1024), init: new Uint8Array(14) }, { delay: async () => {}, ackTimeout: 15 })).rejects.toThrow(error);
      expect(p.received.length).toBe(1);
      expect(p.closed).toBe(true);
      expect(p.readable.locked || p.writable.locked).toBe(false);
    });
  }
  it("bounds DATA retries and identifies the failed data packet", async () => {
    const p = bootloader({ silentFrom: 4 });
    await expect(flashFirmware(p, { binary: new Uint8Array(1024), init: new Uint8Array(14) }, { delay: async () => {}, ackTimeout: 15 }))
      .rejects.toThrow("during DATA (1024/1024 bytes) after 3 attempts. No serial response");
    expect(p.received).toHaveLength(6);
    expect(p.received[4]).toEqual(p.received[3]);
    expect(p.received[5]).toEqual(p.received[3]);
    expect(p.closed).toBe(true);
    expect(p.readable.locked || p.writable.locked).toBe(false);
  });
  it("does not retry STOP after an uncertain activation ACK", async () => {
    const p = bootloader({ silentFrom: 5 });
    await expect(flashFirmware(p, { binary: new Uint8Array(1024), init: new Uint8Array(14) }, { delay: async () => {}, ackTimeout: 15 }))
      .rejects.toThrow("during STOP. No serial response");
    expect(p.received).toHaveLength(5);
    expect(p.closed).toBe(true);
  });
  for (const lost of ["packet", "ACK"]) {
    it(`recovers a lost DATA ${lost} without writing duplicate bytes`, async () => {
      let source, expected = 1, lostOnce = false;
      const received = [], accepted = [], progress = [];
      const port = {
        async open() {
          this.readable = new ReadableStream({ start(c) { source = c; } });
          const decode = slipDecoder(frame => {
            received.push(frame);
            const sequence = frame[0] & 7;
            const opcode = new DataView(frame.buffer).getUint32(4, true);
            const drop = opcode === 4 && !lostOnce;
            if (drop) lostOnce = true;
            if (drop && lost === "packet") return;
            if (sequence === expected) {
              expected = (expected + 1) & 7;
              if (opcode === 4) accepted.push(...frame.subarray(8, -2));
            }
            if (!(drop && lost === "ACK")) source.enqueue(ack(expected));
          });
          this.writable = new WritableStream({ write: decode });
        },
        async setSignals() {}, async close() {},
      };
      const binary = Uint8Array.from({ length: 5000 }, (_, i) => i % 251);
      await flashFirmware(port, { binary, init: new Uint8Array(14) }, {
        delay: async () => {}, ackTimeout: 15,
        onProgress: (state, value) => { if (state === "writing") progress.push(value); },
      });
      expect(new Uint8Array(accepted)).toEqual(binary);
      expect(received[3]).toEqual(received[2]);
      expect(progress).toHaveLength(10);
      expect(progress.at(-1)).toBe(100);
    });
  }
});
