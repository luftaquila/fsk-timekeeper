/* Development-only fake FSK-WL master. Same interface as transport/serial.js.
 *
 * Models what matters to the host: a 16-slot event queue whose head is re-sent every
 * 100 ms until an exactly matching `C`, `H` every second, `D 0` every 5 beacons, a
 * per-sensor `D` + checkpoint every 5 s, checkpoints on demand (every sensor one beacon
 * period after a capture/loss or a `CP`), `T` replies, `K`/`?ID`/`?STATUS`/`PING`.
 * Scenario helpers inject crossings, losses, reboots and a master clock fault.
 */
const TICKS_PER_MS = 16000n;
const QUEUE_LEN = 16;
const RETRY_MS = 100;
const BEACON_MS = 1000; // a checkpoint request rides the next beacon

function rnd32() {
  return Math.floor(Math.random() * 0xffffffff);
}

export function createFakeTransport({ onLine, onDisconnect }) {
  const devid = "F4KEF4KE00000001";
  let masterBoot = rnd32();
  let t0 = 0;
  let tickBase = 0n;
  let seq = 0;
  let evSeq = 0;
  let provisioned = true;
  let open = false;
  let queue = [];
  let timers = [];
  let queueOverflow = 0;
  let masterClockFaulted = false;
  let cpTimer = null; // pending checkpoint request (coalesced until the next beacon)
  let cpRequests = 0; // `CP` commands received
  let autoCheckpoint = true; // the master's own request after a capture/loss (tests switch it off for manual control)
  const sensors = new Map(); // id -> { boot, captureSeq, pendingCount, lastStatusAt, rssi, snr, batt }

  function nowTick() {
    return tickBase + BigInt(Date.now() - t0) * TICKS_PER_MS;
  }
  function emit(line) {
    if (open) onLine?.(line);
  }
  function fmt(n) {
    return Number(n).toFixed(2);
  }

  function addSensor(id) {
    const node = id.toUpperCase();
    if (!sensors.has(node)) {
      sensors.set(node, { boot: rnd32(), captureSeq: 0, pendingCount: 0, lastStatusAt: Date.now(), rssi: -60 - Math.random() * 30, snr: 6 + Math.random() * 6, batt: 3900 + Math.round(Math.random() * 250) });
    }
    return sensors.get(node);
  }

  function enqueue(event) {
    if (queue.length >= QUEUE_LEN) {
      queueOverflow++;
      return false;
    }
    queue.push(event);
    return true;
  }

  function eventLine(e) {
    return `E ${e.node} ${e.ev_seq} ${e.tick} ${e.flags} ${fmt(e.rssi)} ${fmt(e.snr)} ${e.master_boot} ${e.sensor_boot} ${e.capture_seq} ${e.end_seq} ${e.end_tick} ${e.sync_age}`;
  }

  function pump() {
    if (queue.length) emit(eventLine(queue[0]));
  }

  function diagLine(node, s) {
    const age = Date.now() - s.lastStatusAt;
    const state = age <= 10000 ? "OK" : age <= 15000 ? "STALE" : "LOST";
    return `D ${node} ${state} ${-123456} ${12} 0 0 ${age} ${fmt(s.rssi)} ${fmt(s.snr)} 22 ${312} ${s.batt} 0 1 1 1 XTAL ${400 + Math.round(Math.random() * 600)} 0 0 0 0 0 0 ${s.boot} ${masterBoot}`;
  }
  function masterDiag() {
    return `D 0 OK 0 0 0 0 0 0.00 0.00 0 305 4980 0 ${provisioned ? 1 : 0} 1 1 XTAL 0 0 0 ${queue.length} ${queueOverflow} 1 25 0 ${masterBoot}`;
  }
  function identity() {
    return `I FSK-WL 1.0.0 ${devid} 921.30 7 250.00 16000`;
  }
  // GPS/PPS report: latest PPS edge = the last whole second, never before the master booted (tick ≥ 0).
  const gps = { ppb: 0, valid: true, fix: 1, sats: 9, span: 64 };
  function ppsLine() {
    const now = Date.now();
    const edgeMs = Math.max(t0, now - (now % 1000));
    const tick = tickBase + BigInt(edgeMs - t0) * TICKS_PER_MS;
    const valid = gps.valid ? 1 : 0;
    return `P ${tick} ${gps.fix ? Math.floor(edgeMs / 1000) : 0} ${valid ? gps.ppb : 0} ${valid} ${gps.fix} ${gps.sats} ${valid ? gps.span : 0}`;
  }
  function heartbeat() {
    return `H ${nowTick()} ${Date.now() - t0} ${seq % 256} ${sensors.size}`;
  }

  // A checkpoint always lies after every capture it covers (as in the firmware).
  function checkpoint(node) {
    const s = sensors.get(node);
    if (!s) return;
    const at = nowTick();
    const tick = String(s.lastTick != null && s.lastTick >= at ? s.lastTick + TICKS_PER_MS : at);
    enqueue({ node, ev_seq: evSeq++ % 65536, tick, flags: 47, rssi: s.rssi, snr: s.snr, master_boot: masterBoot, sensor_boot: s.boot, capture_seq: s.captureSeq, end_seq: s.captureSeq, end_tick: tick, sync_age: 300 });
  }
  // Checkpoint on demand: the master raises a request after any capture/loss it queues and on
  // `CP`; every sensor answers on the next beacon (the real sensor waits for its own acks first —
  // here the queue already preserves that order).
  function requestCheckpoint() {
    if (cpTimer || !open) return;
    cpTimer = setTimeout(() => {
      cpTimer = null;
      if (!open) return;
      for (const node of sensors.keys()) checkpoint(node);
    }, BEACON_MS);
  }

  // Returns the capture tick (string); `at` pins it exactly instead of now + offsetMs.
  function crossing(node, { offsetMs = 0, flags = 15, at = null } = {}) {
    const s = addSensor(node);
    s.captureSeq = (s.captureSeq + 1) >>> 0;
    const when = at ?? nowTick() + BigInt(offsetMs) * TICKS_PER_MS;
    s.lastTick = s.lastTick != null && s.lastTick > when ? s.lastTick : when;
    const tick = String(when);
    enqueue({ node: node.toUpperCase(), ev_seq: evSeq++ % 65536, tick, flags, rssi: s.rssi, snr: s.snr, master_boot: masterBoot, sensor_boot: s.boot, capture_seq: s.captureSeq, end_seq: s.captureSeq, end_tick: tick, sync_age: 300 });
    s.pendingCount++;
    if (autoCheckpoint) requestCheckpoint();
    return tick;
  }
  function loss(node, n = 1) {
    const s = addSensor(node);
    const from = (s.captureSeq + 1) >>> 0;
    s.captureSeq = (s.captureSeq + n) >>> 0;
    const at = nowTick();
    s.lastTick = s.lastTick != null && s.lastTick > at ? s.lastTick : at;
    const tick = String(at);
    enqueue({ node: node.toUpperCase(), ev_seq: evSeq++ % 65536, tick, flags: 31, rssi: s.rssi, snr: s.snr, master_boot: masterBoot, sensor_boot: s.boot, capture_seq: from, end_seq: s.captureSeq, end_tick: tick, sync_age: 300 });
    if (autoCheckpoint) requestCheckpoint();
  }
  function rebootSensor(node) {
    const s = addSensor(node);
    s.boot = rnd32();
    s.captureSeq = 0;
    s.lastTick = null;
    checkpoint(node.toUpperCase());
  }
  function masterClockFault() {
    const tick = String(nowTick());
    masterClockFaulted = true;
    enqueue({ node: "0", ev_seq: evSeq++ % 65536, tick, flags: 16, rssi: 0, snr: 0, master_boot: masterBoot, sensor_boot: masterBoot, capture_seq: 0, end_seq: 0, end_tick: tick, sync_age: 0 });
  }
  function rebootMaster() {
    masterBoot = rnd32();
    tickBase = 0n;
    t0 = Date.now();
    queue = [];
    masterClockFaulted = false;
    emit(identity());
  }

  function handleCommand(line) {
    const t = line.trim().split(/\s+/);
    switch (t[0]) {
      case "?ID":
        emit(identity());
        emit("A ID OK");
        break;
      case "?STATUS":
        emit(heartbeat());
        for (const [node, s] of sensors) emit(diagLine(node, s));
        emit(masterDiag());
        emit(ppsLine());
        emit("A STATUS OK");
        break;
      case "PING":
        emit("A PING OK");
        break;
      case "CP":
        cpRequests++;
        if (masterClockFaulted) emit("X clock");
        else {
          requestCheckpoint();
          emit("A CP OK");
        }
        break;
      case "K":
        if (/^[0-9a-fA-F]{64}$/.test(t[1] || "")) {
          provisioned = true;
          emit("A K OK");
        } else emit("X keyfail");
        break;
      case "T":
        if (/^[0-9a-f]{32}$/.test(t[1] || "") && !masterClockFaulted) emit(`T ${t[1]} ${nowTick()} ${masterBoot}`);
        else emit("X clock");
        break;
      case "C": {
        const head = queue[0];
        if (head && t[1]?.toUpperCase() === head.node && Number(t[2]) === head.ev_seq && t[3] === head.tick && Number(t[4]) === head.master_boot && Number(t[5]) === head.sensor_boot) {
          queue.shift();
          const s = sensors.get(head.node);
          if (s && !(head.flags & 32) && s.pendingCount > 0) s.pendingCount--;
          pump();
        }
        break;
      }
      default:
        emit("X badcmd");
    }
  }

  async function openTransport() {
    open = true;
    t0 = Date.now();
    tickBase = BigInt(Math.floor(Math.random() * 1e9)) * TICKS_PER_MS;
    if (!sensors.size) {
      addSensor("0D2243B0");
      addSensor("7A1C9F02");
    }
    timers.push(setInterval(pump, RETRY_MS));
    timers.push(
      setInterval(() => {
        seq++;
        emit(heartbeat());
        if (seq % 5 === 0) emit(masterDiag());
        if (!provisioned && seq % 5 === 0) emit("X noprov");
        emit(ppsLine());
      }, 1000),
    );
    timers.push(
      setInterval(() => {
        for (const [node, s] of sensors) {
          s.lastStatusAt = Date.now();
          emit(diagLine(node, s));
          if (s.pendingCount === 0) checkpoint(node);
        }
      }, 5000),
    );
    setTimeout(() => {
      emit(identity());
      for (const [node, s] of sensors) {
        emit(diagLine(node, s));
        checkpoint(node);
      }
      emit(masterDiag());
    }, 50);
    return { usbVendorId: 0x1999, usbProductId: 0x0515, fake: true };
  }

  async function close() {
    open = false;
    for (const t of timers) clearInterval(t);
    timers = [];
    if (cpTimer) clearTimeout(cpTimer);
    cpTimer = null;
  }

  function write(line) {
    if (!open) return Promise.resolve(false);
    setTimeout(() => handleCommand(line), 5);
    return Promise.resolve(true);
  }

  async function enterBootloader() {
    await close();
    onDisconnect?.();
  }

  return {
    kind: "fake",
    open: openTransport,
    close,
    write,
    enterBootloader,
    get connected() {
      return open;
    },
    // scenario helpers
    sensors,
    addSensor,
    crossing,
    checkpoint,
    loss,
    rebootSensor,
    masterClockFault,
    rebootMaster,
    setProvisioned(v) {
      provisioned = !!v;
    },
    // Off: no checkpoint request after a capture/loss (the `CP` command still works).
    setAutoCheckpoint(v) {
      autoCheckpoint = !!v;
    },
    // GPS scenario: { ppb, valid, fix, sats, span }
    setGps(patch) {
      Object.assign(gps, patch);
      if (open) emit(ppsLine());
    },
    get gps() {
      return { ...gps };
    },
    get queueLength() {
      return queue.length;
    },
    get cpRequests() {
      return cpRequests;
    },
  };
}
