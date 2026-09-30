/* FSK-WL USB line protocol v2 (newline-delimited ASCII, one space between tokens).
 *
 * Master -> host:
 *   I FSK-WL <usb_proto> <fw> <devid16> <M|S> <radio_proto> <freq_mhz> <sf> <bw> <ticks_per_ms> <reset_reason>
 *   H <now_tick> <uptime_ms> <beacon_seq> <nseen>
 *   E <hseq> <node> <C|L|K> <ev_seq> <flags> <master_boot_id> <sensor_boot_id> <sync_age_ms> <rssi> <snr>
 *     <capture_seq> <payload...> <crc>   C: <count> <tick>...  L: <end_seq> <tick> <end_tick>  K: <tick>
 *   D <node> <OK|STALE|LOST> <skew_ppm> <rx_miss> <beacon_gap> <last_seen_ms> <rssi> <snr> <lat_ms>
 *     <temp_c10> <batt_mv> <sec_drop> <provisioned> <sync_valid> <skew_valid> <XTAL|RC> <sync_age_ms>
 *     <capture_overflow> <fifo_drop> <queue_depth> <queue_overflow> <err_flags> <ver_drop> <tx_drop>
 *     <reset_reason> <sensor_boot_id> <master_boot_id>
 *   T <request_id> <tick> <master_boot_id>
 *   P <pps_tick> <utc_s|0> <ppb> <pps_valid> <fix> <sats> <span_s> <seg> <n>
 *   A <cmd> OK
 *   X <code> [args...]
 * Host -> master: ?ID ?STATUS PING CP K <64hex> T <32hex> C <hseq> <master_boot_id> <crc>
 *
 * 64-bit ticks stay decimal strings (BigInt on use). Node "0" is the master; sensors are
 * 8 upper-case hex digits. The E line crc covers the text before " <crc>".
 */
import { crc32Hex } from "./crc32";
import { USB_PRODUCT } from "./constants";
import { MASTER_TICKS_PER_MS } from "./event-timing";

export const NODE_MASTER = "0";
export const RADIO_PROTO_VERSION = 11;
export const USB_PROTO_VERSION = 2;

export const FLAG_SYNC = 0x01;
export const FLAG_SKEW = 0x02;
export const FLAG_XTAL = 0x04;
export const FLAG_INTERPOLATED = 0x08; // capture time recovered by interpolation across a sync gap
export const FLAG_TIME_UNKNOWN = 0x40;
export const FLAG_HEALTHY = FLAG_SYNC | FLAG_SKEW | FLAG_XTAL;
const FLAGS_KNOWN = FLAG_HEALTHY | FLAG_INTERPOLATED | FLAG_TIME_UNKNOWN;

export const MAX_SENSORS = 5;
export const EDGES_PER_LINE_MAX = 5;
export const MASTER_QUEUE_CAPACITY = 16;
export const LINK_STALE_MS = 17000;

// Reset-reason byte of the I line and of sensor diagnostics.
export const RESET_BITS = Object.freeze({ PIN: 0x01, DOG: 0x02, SREQ: 0x04, LOCKUP: 0x08, OFF: 0x10, VBUS: 0x20, FAULT: 0x40, OTHER: 0x80 });
const RESET_LABEL = { PIN: "reset pin", DOG: "watchdog", SREQ: "soft reset", LOCKUP: "CPU lockup", OFF: "wake from off", VBUS: "USB wake", FAULT: "fault reboot", OTHER: "other" };

// Sticky per-boot error flags (D line err_flags).
export const ERROR_BITS = Object.freeze({
  RADIO_RESET: 0x0001,
  CAD_TIMEOUT: 0x0002,
  SPI_TIMEOUT: 0x0004,
  TX_FAIL: 0x0008,
  RX_FAIL: 0x0010,
  HFXO_RESTART: 0x0020,
  HW_TIMEOUT: 0x0040,
  FIFO_FULL: 0x0080,
  QUEUE_FULL: 0x0100,
  USB_DROP: 0x0200,
  ACK: 0x0400,
  ID_COLLISION: 0x0800,
});
const ERROR_LABEL = {
  RADIO_RESET: "radio reset",
  CAD_TIMEOUT: "CAD timeout",
  SPI_TIMEOUT: "SPI timeout",
  TX_FAIL: "TX failure",
  RX_FAIL: "RX failure",
  HFXO_RESTART: "HFXO restart",
  HW_TIMEOUT: "hardware timeout",
  FIFO_FULL: "evidence FIFO full",
  QUEUE_FULL: "host queue full",
  USB_DROP: "USB line dropped",
  ACK: "ACK anomaly",
  ID_COLLISION: "sensor id collision",
};

export function errorNames(flags) {
  if (!Number.isInteger(flags)) return [];
  return Object.entries(ERROR_BITS)
    .filter(([, bit]) => flags & bit)
    .map(([name]) => ERROR_LABEL[name]);
}

export function resetNames(reason) {
  if (!Number.isInteger(reason)) return [];
  if (reason === 0) return ["power-on"];
  return Object.entries(RESET_BITS)
    .filter(([, bit]) => reason & bit)
    .map(([name]) => RESET_LABEL[name]);
}

const MASTER_TICK_MAX = (1n << 64n) - 1n;
const U32_MAX = 0xffffffff;

export function validateNodeId(s) {
  return typeof s === "string" && /^[A-Za-z0-9_\-:.]{1,64}$/.test(s);
}

// 64-bit tick as a decimal string or safe integer -> string; undefined when invalid, null when absent.
export function tickToText(v) {
  if (v === undefined || v === null) return null;
  if (typeof v === "number" && Number.isSafeInteger(v) && v >= 0) return String(v);
  if (typeof v === "string" && /^\d{1,20}$/.test(v) && BigInt(v) <= MASTER_TICK_MAX) return v;
  return undefined;
}

export function validBootId(value) {
  return Number.isInteger(value) && value >= 0 && value <= U32_MAX;
}

export function normalizeNodeId(token) {
  const s = String(token ?? "");
  return s === NODE_MASTER ? s : s.toUpperCase();
}

function num(token) {
  if (token === undefined) return NaN;
  return Number(token);
}

// Unsigned decimal token no larger than max, else null.
function uint(token, max) {
  if (typeof token !== "string" || !/^\d{1,20}$/.test(token)) return null;
  const v = Number(token);
  return Number.isSafeInteger(v) && v <= max ? v : null;
}

function stateMap(s) {
  return s === "OK" ? "online" : s === "STALE" ? "degraded" : s === "LOST" ? "lost" : null;
}

// One line -> typed message, or null for blank input. Unknown prefixes yield { type: "?" }.
export function parseLine(line) {
  const text = String(line ?? "").trim();
  const t = text.split(/\s+/);
  if (!t[0]) return null;
  switch (t[0]) {
    case "E":
      return { type: "E", raw: text };
    case "D":
      if (t.length !== 28) return { type: "D", telemetry: null };
      return {
        type: "D",
        telemetry: {
          node_id: normalizeNodeId(t[1]),
          link_state: stateMap(t[2]),
          skew_ppm: num(t[3]),
          rx_miss: num(t[4]),
          beacon_gap: num(t[5]),
          last_seen_ms: num(t[6]),
          rssi: num(t[7]),
          snr: num(t[8]),
          latency_ms: num(t[9]),
          temp_c10: num(t[10]),
          batt_mv: num(t[11]),
          sec_drop: num(t[12]),
          provisioned: num(t[13]),
          sync_valid: num(t[14]),
          skew_valid: num(t[15]),
          clock_source: t[16] === "XTAL" ? "xtal" : t[16] === "RC" ? "rc" : null,
          sync_age_ms: num(t[17]),
          capture_overflow: num(t[18]),
          fifo_drop: num(t[19]),
          queue_depth: num(t[20]),
          queue_overflow: num(t[21]),
          err_flags: num(t[22]),
          ver_drop: num(t[23]),
          tx_drop: num(t[24]),
          reset_reason: num(t[25]),
          sensor_boot_id: num(t[26]),
          master_boot_id: num(t[27]),
        },
      };
    case "H":
      return { type: "H", nowTick: t[1], uptimeMs: num(t[2]), beaconSeq: num(t[3]), nseen: num(t[4]) };
    case "T":
      return { type: "T", requestId: t[1], masterTick: t[2], masterBootId: num(t[3]) };
    case "P":
      if (t.length !== 10) return { type: "P", pps: null };
      return {
        type: "P",
        pps: { tick: t[1], utc: num(t[2]), ppb: num(t[3]), valid: num(t[4]), fix: num(t[5]), sats: num(t[6]), span: num(t[7]), seg: num(t[8]), n: num(t[9]) },
      };
    case "I": {
      // v1 boards put the firmware version where v2 has the protocol number.
      const v1 = !/^\d+$/.test(t[2] || "");
      if (v1) return { type: "I", product: t[1], usbProto: null, fw: t[2] ?? null, devid: t[3] ?? null, role: null, radioProto: null, ticksPerMs: num(t[7]), v1: true };
      return {
        type: "I",
        product: t[1],
        usbProto: num(t[2]),
        fw: t[3],
        devid: t[4],
        role: t[5],
        radioProto: num(t[6]),
        freqMhz: num(t[7]),
        sf: num(t[8]),
        bw: num(t[9]),
        ticksPerMs: num(t[10]),
        resetReason: num(t[11]),
        v1: false,
      };
    }
    case "A":
      return { type: "A", cmd: t[1] };
    case "X":
      return { type: "X", reason: t[1], args: t.slice(2), text };
    default:
      return { type: "?", raw: line };
  }
}

export const CONTRACT_SENSOR_ROLE = "This board booted as a sensor — reset it while it is connected to the PC.";
export const CONTRACT_VERSION = "Firmware and console versions do not match — update both.";

// The master may be used for measurement only after an I line of this exact contract.
export function contractOf(identity) {
  if (!identity) return { ok: false, reason: "Waiting for the master identity (?ID)." };
  if (identity.product !== USB_PRODUCT) return { ok: false, reason: `Unexpected device: ${identity.product}.` };
  if (identity.usbProto !== USB_PROTO_VERSION || identity.radioProto !== RADIO_PROTO_VERSION || identity.ticksPerMs !== Number(MASTER_TICKS_PER_MS)) {
    return { ok: false, reason: CONTRACT_VERSION };
  }
  if (identity.role === "S") return { ok: false, reason: CONTRACT_SENSOR_ROLE };
  if (identity.role !== "M") return { ok: false, reason: CONTRACT_VERSION };
  return { ok: true, reason: null };
}

export function lineKey(masterBootId, hseq) {
  return `${masterBootId}:${hseq}`;
}

// Dedupe key of one evidence row (a C line expands into one row per edge).
export function rowKey(row) {
  return `${row.master_boot_id}:${row.node_id}:${row.sensor_boot_id}:${row.kind}:${row.capture_seq}:${row.master_tick}`;
}

/* One E line -> { stage, ... }:
 *   "unreadable": hseq or crc cannot be read — never acked.
 *   "crc":        crc mismatch — never acked, the master re-sends.
 *   "invalid":    the master really sent this, but it breaks the contract — quarantined + acked.
 *   "valid":      rows ready to store (without seq / received_at).
 */
export function parseEventLine(raw) {
  const line = String(raw ?? "").replace(/\s+$/, "");
  const t = line.split(" ");
  const hseq = t.length >= 3 ? uint(t[1], U32_MAX) : null;
  const crcToken = t.length >= 3 ? t[t.length - 1] : "";
  if (t[0] !== "E" || hseq == null || hseq < 1 || !/^[0-9A-Fa-f]{8}$/.test(crcToken)) return { stage: "unreadable", raw: line };
  const body = line.slice(0, line.length - crcToken.length - 1);
  const crc = crcToken.toUpperCase();
  if (/[^\x20-\x7e]/.test(body) || crc32Hex(body) !== crc) return { stage: "crc", raw: line, hseq, crc };

  const masterBootId = uint(t[6], U32_MAX);
  const nodeToken = t[2] ?? "";
  const node = nodeToken === NODE_MASTER || /^[0-9A-Fa-f]{8}$/.test(nodeToken) ? normalizeNodeId(nodeToken) : null;
  const invalid = (reason) => ({ stage: "invalid", raw: line, hseq, crc, masterBootId, node, reason });
  if (masterBootId == null) return invalid("master_boot_id");
  if (node == null) return invalid("node");
  const kind = t[3];
  const evSeq = uint(t[4], 0xffff);
  const flags = uint(t[5], 0xff);
  const sensorBootId = uint(t[7], U32_MAX);
  const syncAge = uint(t[8], 0xffff);
  const rssi = /^-?\d+(\.\d+)?$/.test(t[9] ?? "") ? Number(t[9]) : NaN;
  const snr = /^-?\d+(\.\d+)?$/.test(t[10] ?? "") ? Number(t[10]) : NaN;
  const captureSeq = uint(t[11], U32_MAX);
  if (!["C", "L", "K"].includes(kind)) return invalid("kind");
  if (evSeq == null) return invalid("ev_seq");
  if (flags == null || flags & ~FLAGS_KNOWN) return invalid("flags");
  if (sensorBootId == null) return invalid("sensor_boot_id");
  if (syncAge == null) return invalid("sync_age_ms");
  if (!Number.isFinite(rssi) || !Number.isFinite(snr)) return invalid("rssi/snr");
  if (captureSeq == null) return invalid("capture_seq");
  const payload = t.slice(12, t.length - 1);
  const base = {
    hseq,
    node_id: node,
    ev_seq: evSeq,
    flags,
    master_boot_id: masterBootId,
    sensor_boot_id: sensorBootId,
    sync_age_ms: syncAge,
    rssi,
    snr,
    raw: line,
    usb_proto: USB_PROTO_VERSION,
  };
  const tickOf = (token) => (tickToText(token) ? token : null);
  let rows;
  if (kind === "C") {
    const count = uint(payload[0], EDGES_PER_LINE_MAX);
    if (count == null || count < 1 || payload.length !== count + 1) return invalid("edge count");
    const ticks = payload.slice(1).map(tickOf);
    if (ticks.some((x) => x == null)) return invalid("edge tick");
    for (let i = 1; i < ticks.length; i++) if (BigInt(ticks[i]) <= BigInt(ticks[i - 1])) return invalid("edge order");
    rows = ticks.map((tick, i) => {
      const seq = (captureSeq + i) >>> 0;
      return { ...base, kind: "capture", capture_seq: seq, end_seq: seq, master_tick: tick, end_tick: tick };
    });
  } else if (kind === "L") {
    const endSeq = uint(payload[0], U32_MAX);
    const tick = tickOf(payload[1]);
    const endTick = tickOf(payload[2]);
    if (payload.length !== 3 || endSeq == null || tick == null || endTick == null) return invalid("loss payload");
    if ((endSeq - captureSeq) >>> 0 >= 0x80000000) return invalid("loss range");
    if (!(flags & FLAG_TIME_UNKNOWN) && BigInt(endTick) < BigInt(tick)) return invalid("loss ticks");
    rows = [{ ...base, kind: "loss", capture_seq: captureSeq, end_seq: endSeq, master_tick: tick, end_tick: endTick }];
  } else {
    const tick = tickOf(payload[0]);
    if (payload.length !== 1 || tick == null) return invalid("checkpoint payload");
    rows = [{ ...base, kind: "checkpoint", capture_seq: captureSeq, end_seq: captureSeq, master_tick: tick, end_tick: tick }];
  }
  if (node === NODE_MASTER && (kind !== "L" || evSeq !== 0 || flags !== 0 || captureSeq !== 0 || rows[0].end_seq !== 0)) return invalid("master record");
  return { stage: "valid", raw: line, hseq, crc, masterBootId, node, kind, rows };
}

// Host commit of the master's queue head.
export function formatAck({ hseq, masterBootId, crc }) {
  return `C ${hseq} ${masterBootId} ${crc}`;
}

function intOrNull(v) {
  return Number.isFinite(v) ? Math.trunc(v) : null;
}
function nonNegOrNull(v) {
  return Number.isFinite(v) ? Math.max(0, Math.trunc(v)) : null;
}
function bitOrNull(v) {
  return v === 0 || v === 1 ? v : null;
}

// D line -> telemetry. `received_at` dates the report; `last_seen_at` is when the master
// last heard the node (informational — the link state is the firmware's).
export function normalizeTelemetry(t, now = Date.now()) {
  if (!t || !validateNodeId(String(t.node_id))) return null;
  const heardAgeMs = Number.isFinite(t.last_seen_ms) && t.last_seen_ms >= 0 ? Math.trunc(t.last_seen_ms) : null;
  return {
    node_id: String(t.node_id),
    link_state: ["online", "degraded", "lost"].includes(t.link_state) ? t.link_state : null,
    rssi: Number.isFinite(t.rssi) ? t.rssi : null,
    snr: Number.isFinite(t.snr) ? t.snr : null,
    skew_ppm: Number.isFinite(t.skew_ppm) ? t.skew_ppm : null,
    latency_ms: Number.isFinite(t.latency_ms) ? t.latency_ms : null,
    rx_miss: intOrNull(t.rx_miss),
    beacon_gap: intOrNull(t.beacon_gap),
    temp_c10: intOrNull(t.temp_c10),
    batt_mv: intOrNull(t.batt_mv),
    sec_drop: intOrNull(t.sec_drop),
    provisioned: bitOrNull(t.provisioned),
    sync_valid: bitOrNull(t.sync_valid),
    skew_valid: bitOrNull(t.skew_valid),
    clock_source: ["xtal", "rc"].includes(t.clock_source) ? t.clock_source : null,
    sync_age_ms: nonNegOrNull(t.sync_age_ms),
    capture_overflow: nonNegOrNull(t.capture_overflow),
    fifo_drop: nonNegOrNull(t.fifo_drop),
    queue_depth: nonNegOrNull(t.queue_depth),
    queue_overflow: nonNegOrNull(t.queue_overflow),
    err_flags: nonNegOrNull(t.err_flags),
    ver_drop: nonNegOrNull(t.ver_drop),
    tx_drop: nonNegOrNull(t.tx_drop),
    reset_reason: nonNegOrNull(t.reset_reason),
    sensor_boot_id: validBootId(t.sensor_boot_id) ? t.sensor_boot_id : null,
    master_boot_id: validBootId(t.master_boot_id) ? t.master_boot_id : null,
    last_seen_at: heardAgeMs === null ? null : now - heardAgeMs,
    received_at: now,
  };
}

// P line -> GPS report. seg >= 1 marks a qualified edge (n = its index in the segment).
export function normalizePps(p, now = Date.now()) {
  if (!p) return null;
  const tick = tickToText(p.tick);
  if (tick == null) return null;
  const seg = nonNegOrNull(p.seg) ?? 0;
  return {
    tick,
    utc: Number.isInteger(p.utc) && p.utc > 0 ? p.utc : null,
    ppb: Number.isInteger(p.ppb) ? p.ppb : 0,
    valid: bitOrNull(p.valid) ?? 0,
    fix: nonNegOrNull(p.fix) ?? 0,
    sats: nonNegOrNull(p.sats) ?? 0,
    span: nonNegOrNull(p.span) ?? 0,
    seg,
    n: seg ? nonNegOrNull(p.n) ?? 0 : 0,
    at: now,
  };
}

export function isHexKey(s) {
  return typeof s === "string" && /^[0-9a-fA-F]{64}$/.test(s);
}
