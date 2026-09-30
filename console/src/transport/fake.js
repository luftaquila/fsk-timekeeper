/* Development fake of the FSK-WL master, USB line protocol v2. Same interface as serial.js.
 *
 * Each sensor keeps its records (captures, losses, checkpoints) in order; a record reaches the
 * master queue one slot delay after it was made, consecutive captures bundled up to 5 per E
 * line. The master queue (16 lines) re-sends its head every 100 ms until an exact
 * `C <hseq> <master_boot_id> <crc>`. `H` every second, `D 0` every 5 s, a sensor `D` with every
 * uplink and every 5 s, a periodic checkpoint per idle sensor every 5 s, checkpoints one beacon
 * after `CP`, one `P` line per PPS edge with seg/n. Scenario helpers inject the rest.
 */
import { crc32Hex } from "../lib/crc32";

const TICKS_PER_MS = 16000n;
const QUEUE_LEN = 16;
const RETRY_MS = 100;
const SLOT_POLL_MS = 20;
const BEACON_MS = 1000;
const STATUS_MS = 5000;
const HEALTHY = 7;
const TIME_UNKNOWN = 64;

// Defaults for new fakes (tests shorten the slot delay before connecting).
export const fakeDefaults = { slotDelayMin: 300, slotDelayMax: 1000, periodicCheckpoints: true };

function rnd32() {
  return Math.floor(Math.random() * 0xffffffff);
}

export function createFakeTransport({ onLine, onDisconnect }) {
  const devid = "F4KEF4KE00000001";
  let role = "M";
  const proto = { usb: 2, radio: 11, ticks: 16000 };
  let masterBoot = rnd32();
  let t0 = 0;
  let tickBase = 0n;
  let beaconSeq = 0;
  let hseq = 0;
  let provisioned = true;
  let open = false;
  let queue = []; // { hseq, mboot, crc, line }
  let timers = [];
  let queueOverflow = 0;
  let queueDepthOverride = null;
  let masterClockFaulted = false;
  let cpTimer = null;
  let cpRequests = 0;
  let corruptOnce = false;
  let corruptAlways = false;
  let errFlags = 0;
  let verDrop = 0;
  let txDrop = 0;
  const slotDelay = { min: fakeDefaults.slotDelayMin, max: fakeDefaults.slotDelayMax };
  let periodicCheckpoints = fakeDefaults.periodicCheckpoints;
  const sensors = new Map(); // id -> sensor state

  // GPS: PPS edge k of the current anchor is at anchorTick + k·period (period follows ppb).
  const gps = { ppb: 0, valid: true, fix: 1, sats: 9, span: 64 };
  const pps = { anchorTick: 0n, k: 0, seg: 1, n: 0, utc0: 0, emitted: 0, last: null };

  function nowTick() {
    return tickBase + BigInt(Date.now() - t0) * TICKS_PER_MS;
  }
  function emit(line) {
    if (open) onLine?.(line);
  }
  function fmt(n) {
    return Number(n).toFixed(2);
  }
  function delay() {
    return slotDelay.min + Math.random() * Math.max(0, slotDelay.max - slotDelay.min);
  }

  function addSensor(id) {
    const node = id.toUpperCase();
    if (!sensors.has(node)) {
      sensors.set(node, {
        boot: rnd32(),
        captureSeq: 0,
        evSeq: 0,
        records: [], // { kind, seq, endSeq, tick, endTick, flags, due }
        lastTick: null,
        lastUplinkAt: Date.now(),
        rssi: -60 - Math.random() * 30,
        snr: 6 + Math.random() * 6,
        batt: 3900 + Math.round(Math.random() * 250),
      });
    }
    return sensors.get(node);
  }

  function push(s, record, immediate = false) {
    s.records.push({ ...record, due: Date.now() + (immediate ? 0 : delay()) });
  }

  // ---- master queue ---------------------------------------------------------
  function enqueue(body) {
    if (queue.length >= QUEUE_LEN) {
      queueOverflow++;
      return false;
    }
    hseq += 1;
    const text = body(hseq);
    const crc = crc32Hex(text);
    queue.push({ hseq, mboot: masterBoot, crc, line: `${text} ${crc}` });
    return true;
  }

  function pump() {
    const head = queue[0];
    if (!head) return;
    if (corruptOnce || corruptAlways) {
      corruptOnce = false;
      emit(head.line.replace(/ (\d)(\d*) (\S+)$/, (m, a, b, crc) => ` ${(Number(a) + 1) % 10}${b} ${crc}`));
      return;
    }
    emit(head.line);
  }

  function uplink(node, s) {
    const head = s.records[0];
    if (!head || head.due > Date.now()) return;
    const common = (h, kind, flags, seq) => `E ${h} ${node} ${kind} ${s.evSeq % 65536} ${flags} ${masterBoot} ${s.boot} 300 ${fmt(s.rssi)} ${fmt(s.snr)} ${seq}`;
    let count = 1;
    let body;
    if (head.kind === "capture") {
      while (
        count < 5 &&
        s.records[count]?.kind === "capture" &&
        s.records[count].due <= Date.now() &&
        s.records[count].flags === head.flags &&
        s.records[count].seq === ((head.seq + count) >>> 0)
      ) {
        count++;
      }
      const ticks = s.records.slice(0, count).map((r) => String(r.tick));
      body = (h) => `${common(h, "C", head.flags, head.seq)} ${count} ${ticks.join(" ")}`;
    } else if (head.kind === "loss") {
      body = (h) => `${common(h, "L", head.flags, head.seq)} ${head.endSeq} ${head.tick} ${head.endTick}`;
    } else if (head.kind === "bad") {
      body = (h) => `${common(h, "Z", head.flags, head.seq)} 1 ${head.tick}`;
    } else {
      body = (h) => `${common(h, "K", head.flags, head.seq)} ${head.tick}`;
    }
    if (!enqueue(body)) return; // host queue full: the sensor keeps the records (backpressure)
    s.records.splice(0, count);
    s.evSeq++;
    s.lastUplinkAt = Date.now();
    emit(diagLine(node, s));
  }

  function slots() {
    for (const [node, s] of sensors) uplink(node, s);
    emitPps();
  }

  // ---- lines ------------------------------------------------------------------
  function diagLine(node, s) {
    const age = Date.now() - s.lastUplinkAt;
    const state = age <= 12000 ? "OK" : age <= 17000 ? "STALE" : "LOST";
    return `D ${node} ${state} 12 0 0 ${age} ${fmt(s.rssi)} ${fmt(s.snr)} 22 312 ${s.batt} 0 1 1 1 XTAL ${400 + Math.round(Math.random() * 600)} 0 0 0 0 0 0 0 0 ${s.boot} ${masterBoot}`;
  }
  function masterDiag() {
    const depth = queueDepthOverride ?? queue.length;
    return `D 0 OK 0 0 0 0 0.00 0.00 0 305 4980 0 ${provisioned ? 1 : 0} 1 1 XTAL 0 0 0 ${depth} ${queueOverflow} ${errFlags} ${verDrop} ${txDrop} 0 0 ${masterBoot}`;
  }
  function identity() {
    return `I FSK-WL ${proto.usb} 1.0.0 ${devid} ${role} ${proto.radio} 921.30 7 250.00 ${proto.ticks} 0`;
  }
  function heartbeat() {
    return `H ${nowTick()} ${Date.now() - t0} ${beaconSeq % 256} ${sensors.size}`;
  }

  function ppsPeriod() {
    return (16_000_000n * BigInt(1_000_000_000 + gps.ppb)) / 1_000_000_000n;
  }
  function ppsLine(edge) {
    return `P ${edge.tick} ${edge.utc} ${edge.valid ? gps.ppb : 0} ${edge.valid ? 1 : 0} ${gps.fix} ${gps.sats} ${edge.valid ? gps.span : 0} ${edge.seg} ${edge.n}`;
  }
  // Emit every PPS edge that has happened by now.
  function emitPps() {
    for (;;) {
      const tick = pps.anchorTick + BigInt(pps.k + 1) * ppsPeriod();
      if (tick > nowTick()) return;
      pps.k += 1;
      pps.emitted += 1;
      const valid = gps.valid;
      const edge = { tick, utc: gps.fix ? pps.utc0 + pps.emitted : 0, valid, seg: valid ? pps.seg : 0, n: valid ? pps.n : 0 };
      if (valid) pps.n += 1;
      pps.last = edge;
      emit(ppsLine(edge));
    }
  }
  // Re-anchor at the latest edge so a ppb change applies from there on.
  function reanchor() {
    pps.anchorTick = pps.last ? pps.last.tick : pps.anchorTick + BigInt(pps.k) * ppsPeriod();
    pps.k = 0;
  }

  // A checkpoint always lies after every capture it covers (as in the firmware).
  function checkpoint(node, { immediate = false } = {}) {
    const s = sensors.get(node.toUpperCase());
    if (!s) return;
    const at = nowTick();
    const tick = s.lastTick != null && s.lastTick >= at ? s.lastTick + TICKS_PER_MS : at;
    push(s, { kind: "checkpoint", seq: s.captureSeq, tick, flags: HEALTHY }, immediate);
  }
  function requestCheckpoint() {
    if (cpTimer || !open) return;
    cpTimer = setTimeout(() => {
      cpTimer = null;
      if (!open) return;
      for (const node of sensors.keys()) checkpoint(node, { immediate: true });
    }, BEACON_MS);
  }

  // Returns the capture tick (string); `at` pins it exactly instead of now + offsetMs.
  function crossing(node, { offsetMs = 0, flags = HEALTHY, at = null } = {}) {
    const s = addSensor(node);
    s.captureSeq = (s.captureSeq + 1) >>> 0;
    const when = at != null ? BigInt(at) : nowTick() + BigInt(offsetMs) * TICKS_PER_MS;
    s.lastTick = s.lastTick != null && s.lastTick > when ? s.lastTick : when;
    push(s, { kind: "capture", seq: s.captureSeq, tick: when, flags });
    return String(when);
  }
  // n lost captures; known time by default (the range [at, at + spanMs]).
  function loss(node, n = 1, { unknownTime = false, at = null, spanMs = 0 } = {}) {
    const s = addSensor(node);
    const from = (s.captureSeq + 1) >>> 0;
    s.captureSeq = (s.captureSeq + n) >>> 0;
    const first = at != null ? BigInt(at) : nowTick();
    const last = first + BigInt(spanMs) * TICKS_PER_MS;
    s.lastTick = s.lastTick != null && s.lastTick > last ? s.lastTick : last;
    push(s, { kind: "loss", seq: from, endSeq: s.captureSeq, tick: unknownTime ? 0n : first, endTick: unknownTime ? 0n : last, flags: unknownTime ? TIME_UNKNOWN : HEALTHY });
  }
  function rebootSensor(node) {
    const s = addSensor(node);
    s.boot = rnd32();
    s.captureSeq = 0;
    s.records = [];
    s.lastTick = null;
    checkpoint(node, { immediate: true });
  }
  // An event whose crc is right but whose content is not (quarantined by the console); it
  // stands for one capture record, so the sensor's next record shows the gap.
  function injectBadLine(node) {
    const s = addSensor(node);
    s.captureSeq = (s.captureSeq + 1) >>> 0;
    push(s, { kind: "bad", seq: s.captureSeq, tick: nowTick(), flags: HEALTHY }, true);
  }
  function masterClockFault() {
    const tick = String(nowTick());
    masterClockFaulted = true;
    enqueue((h) => `E ${h} 0 L 0 0 ${masterBoot} ${masterBoot} 0 0.00 0.00 0 0 ${tick} ${tick}`);
  }
  function rebootMaster() {
    masterBoot = rnd32();
    tickBase = 0n;
    t0 = Date.now();
    queue = [];
    hseq = 0;
    masterClockFaulted = false;
    for (const s of sensors.values()) s.records = [];
    pps.anchorTick = 0n;
    pps.k = 0;
    pps.seg = 1;
    pps.n = 0;
    pps.last = null;
    emit(identity());
    emit(masterDiag());
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
        if (pps.last) emit(ppsLine(pps.last));
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
        if (head && Number(t[1]) === head.hseq && Number(t[2]) === head.mboot && (t[3] || "").toUpperCase() === head.crc) {
          queue.shift();
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
    pps.anchorTick = tickBase;
    pps.utc0 = Math.floor(t0 / 1000);
    if (!sensors.size) {
      addSensor("0D2243B0");
      addSensor("7A1C9F02");
    }
    timers.push(setInterval(pump, RETRY_MS));
    timers.push(setInterval(slots, SLOT_POLL_MS));
    timers.push(
      setInterval(() => {
        beaconSeq++;
        emit(heartbeat());
        if (beaconSeq % 5 === 0) emit(masterDiag());
        if (!provisioned && beaconSeq % 5 === 0) emit("X noprov");
      }, BEACON_MS),
    );
    timers.push(
      setInterval(() => {
        for (const [node, s] of sensors) {
          emit(diagLine(node, s));
          if (periodicCheckpoints && !s.records.length) checkpoint(node);
        }
      }, STATUS_MS),
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
    injectBadLine,
    masterClockFault,
    rebootMaster,
    setProvisioned(v) {
      provisioned = !!v;
    },
    setSlotDelay(min, max = min) {
      slotDelay.min = min;
      slotDelay.max = max;
    },
    setPeriodicCheckpoints(v) {
      periodicCheckpoints = !!v;
    },
    corruptNext() {
      corruptOnce = true;
    },
    setCorrupt(v) {
      corruptAlways = !!v;
    },
    setRole(r) {
      role = r;
      emit(identity());
    },
    setProto(patch) {
      Object.assign(proto, patch);
      emit(identity());
    },
    setMasterDiag({ errFlags: e, verDrop: v, txDrop: x, queueDepth } = {}) {
      if (e != null) errFlags = e;
      if (v != null) verDrop = v;
      if (x != null) txDrop = x;
      if (queueDepth !== undefined) queueDepthOverride = queueDepth;
      emit(masterDiag());
    },
    emitError(code, count = 1) {
      emit(`X ${code} ${count}`);
    },
    // GPS scenario: { ppb, valid, fix, sats, span }
    setGps(patch) {
      reanchor();
      if (patch.valid === false && gps.valid) pps.seg += 1; // qualification ends: the next valid edge opens a new segment
      if (patch.valid === false) pps.n = 0;
      Object.assign(gps, patch);
    },
    breakGpsSegment() {
      pps.seg += 1;
      pps.n = 0;
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
    get masterBootId() {
      return masterBoot;
    },
    nowTick,
  };
}
