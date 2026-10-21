/* Adafruit nRF52 legacy serial DFU (HCI over SLIP), not Nordic Secure DFU; wire reference:
 * Adafruit_nRF52_nrfutil/nordicsemi/dfu/dfu_transport_serial.py. The first sequence sent is 1;
 * an ACK carries the next expected sequence. */
export function crc16(bytes) {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
  }
  return crc;
}

export function words(...values) {
  const bytes = new Uint8Array(values.length * 4);
  const view = new DataView(bytes.buffer);
  values.forEach((v, i) => view.setUint32(i * 4, v, true));
  return bytes;
}

export function packet(payload, sequence) {
  const bytes = new Uint8Array(payload.length + 6);
  bytes[0] = sequence | (((sequence + 1) & 7) << 3) | 0xc0;
  bytes[1] = 0x0e | ((payload.length & 15) << 4);
  bytes[2] = payload.length >> 4;
  bytes[3] = -(bytes[0] + bytes[1] + bytes[2]) & 255;
  bytes.set(payload, 4);
  const crc = crc16(bytes.subarray(0, -2));
  bytes[bytes.length - 2] = crc & 255;
  bytes[bytes.length - 1] = crc >> 8;
  const encoded = [0xc0];
  for (const byte of bytes) encoded.push(...(byte === 0xc0 ? [0xdb, 0xdc] : byte === 0xdb ? [0xdb, 0xdd] : [byte]));
  encoded.push(0xc0);
  return new Uint8Array(encoded);
}

export function ackNumber(frame) {
  if (frame.length < 4 || (frame[0] + frame[1] + frame[2] + frame[3]) % 256 !== 0) throw new Error("Invalid DFU ACK header.");
  const size = (frame[1] >> 4) | (frame[2] << 4);
  const hasCrc = !!(frame[0] & 0x40);
  if ((frame[1] & 15) !== 0 || size !== 0 || frame.length !== 4 + (hasCrc ? 2 : 0)) throw new Error("Unexpected DFU response.");
  if (hasCrc && crc16(frame.subarray(0, -2)) !== (frame.at(-2) | (frame.at(-1) << 8))) throw new Error("Invalid DFU ACK checksum.");
  return (frame[0] >> 3) & 7;
}

export function slipDecoder(onFrame) {
  let bytes = [], escaped = false, started = false;
  return (chunk) => {
    for (const byte of chunk) {
      if (byte === 0xc0) {
        if (escaped) throw new Error("Invalid SLIP escape.");
        if (started && bytes.length) onFrame(new Uint8Array(bytes));
        bytes = []; started = true;
      } else if (started) {
        if (escaped) {
          if (byte !== 0xdc && byte !== 0xdd) throw new Error("Invalid SLIP escape.");
          bytes.push(byte === 0xdc ? 0xc0 : 0xdb); escaped = false;
        } else if (byte === 0xdb) escaped = true;
        else bytes.push(byte);
        if (bytes.length > 1024) throw new Error("Oversized DFU response.");
      }
    }
  };
}

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function flashFirmware(port, firmware, { onProgress = () => {}, delay = sleep, ackTimeout = 2500 } = {}) {
  let reader, writer, reading, pending, failure, closing = false, opened = false;
  let sequence = 0;
  const fail = (error) => { failure = error; pending?.reject(error); };
  try {
    await port.open({ baudRate: 115200, flowControl: "none" });
    opened = true;
    reader = port.readable.getReader();
    writer = port.writable.getWriter();
    const decode = slipDecoder(frame => {
      const ack = ackNumber(frame);
      if (pending) pending.lastAck = ack;
      if (pending && ack === pending.expected) pending.resolve();
      // Other valid ACKs can be delayed duplicates. They must not advance progress.
    });
    reading = (async () => {
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) { if (!closing) fail(new Error("Bootloader disconnected.")); break; }
          if (value) {
            if (pending) pending.receivedBytes += value.length;
            decode(value);
          }
        }
      } catch (e) { if (!closing) fail(e); }
    })();
    // nrfutil's port-open settling time, also for a bootloader selected right after enumeration.
    await delay(100);
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    await delay(50);
    await port.setSignals({ dataTerminalReady: true });
    await delay(100);
    const send = async (payload, stage, attempts = 1) => {
      if (failure) throw failure;
      sequence = (sequence + 1) & 7;
      const encoded = packet(payload, sequence);
      // HCI drops duplicate sequences and re-sends its ACK: retry DATA with the identical frame so
      // a lost ACK cannot duplicate bytes. START/STOP are not retried (they erase flash or reset).
      for (let attempt = 1; attempt <= attempts; attempt++) {
        let timer;
        const acknowledged = new Promise((resolve, reject) => {
          pending = { expected: (sequence + 1) & 7, resolve, reject, lastAck: null, receivedBytes: 0 };
          timer = setTimeout(() => {
            const response = pending.lastAck !== null
              ? `Expected ACK ${pending.expected}, received ${pending.lastAck}`
              : pending.receivedBytes ? `Received ${pending.receivedBytes} bytes without a complete ACK` : "No serial response";
            const error = new Error(`Timed out waiting for bootloader ACK during ${stage}${attempts > 1 ? ` after ${attempts} attempts` : ""}. ${response}. Double-tap RST or reconnect USB, then retry.`);
            error.code = "DFU_ACK_TIMEOUT";
            reject(error);
          }, ackTimeout);
        });
        try {
          await Promise.all([writer.write(encoded), acknowledged]);
          return;
        } catch (e) {
          if (failure || e.code !== "DFU_ACK_TIMEOUT" || attempt === attempts) throw e;
        }
        finally { clearTimeout(timer); pending = null; }
      }
    };
    onProgress("erasing", 0);
    await send(words(3, 4, 0, 0, firmware.binary.length), "START");
    await delay(Math.max(500, (Math.floor(firmware.binary.length / 4096) + 1) * 89.7));
    await send(new Uint8Array([...words(1), ...firmware.init, 0, 0]), "INIT");
    for (let offset = 0; offset < firmware.binary.length; offset += 512) {
      const data = firmware.binary.subarray(offset, offset + 512);
      await send(new Uint8Array([...words(4), ...data]), `DATA (${offset + data.length}/${firmware.binary.length} bytes)`, 3);
      onProgress("writing", Math.floor((offset + data.length) * 100 / firmware.binary.length));
      if ((offset / 512) % 8 === 0) await delay(103);
    }
    await delay(103);
    onProgress("restarting", 100);
    await send(words(5), "STOP");
    await delay(300); // bootloader settings page save before opening the app
  } finally {
    closing = true;
    try { await reader?.cancel(); } catch { /* device re-enumerated */ }
    await reading;
    reader?.releaseLock();
    writer?.releaseLock();
    if (opened) {
      try { await port.close(); } catch { /* device re-enumerated */ }
    }
  }
}
