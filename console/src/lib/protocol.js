/* FSK-WL USB line protocol (newline-delimited ASCII).
 *
 * Master -> host:
 *   I FSK-WL <fw> <devid16hex> <freq_mhz> <sf> <bw> <ticks_per_ms>
 *   H <now_tick> <uptime_ms> <beacon_seq> <nseen>
 *   E <node> <ev_seq> <tick> <flags> <rssi> <snr> <master_boot_id> <sensor_boot_id>
 *     <capture_seq> <end_seq> <end_tick> <sync_age_ms>
 *   D <node> <OK|STALE|LOST> <offset_tick> <skew_ppm> <rx_miss> <beacon_gap> <last_seen_ms>
 *     <rssi> <snr> <lat_ms> <temp_c10> <batt_mv> <sec_drop> <provisioned> <sync_valid>
 *     <skew_valid> <XTAL|RC> <sync_age_ms> <capture_overflow> <event_drop> <queue_depth>
 *     <queue_overflow> <usb_ref_valid> <usb_ref_ppm> <sensor_boot_id> <master_boot_id>
 *   T <request_id> <tick> <master_boot_id>
 *   P <pps_tick> <utc_s|0> <ppb> <pps_valid> <fix> <sats> <span_s>   (master with GPS, ~1 Hz)
 *   A <cmd> OK
 *   X <reason>
 * Host -> master: ?ID ?STATUS PING K<64hex> T<32hex> C <node> <ev_seq> <tick> <mboot> <sboot>
 *
 * 64-bit ticks stay decimal strings (BigInt on use). Node "0" is the master,
 * sensors are 8 hex digits (normalised to upper case here).
 */
import { CAPTURE_CHECKPOINT, CAPTURE_LOSS } from "./capture-integrity";

export const NODE_MASTER = "0";
const MASTER_TICK_MAX = (1n << 64n) - 1n;

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
  return Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
}

export const ALLOWED_ROLE = /^(start|finish)$/;

export function normalizeNodeId(token) {
  const s = String(token ?? "");
  return s === NODE_MASTER ? s : s.toUpperCase();
}

function num(token) {
  if (token === undefined) return NaN;
  return Number(token);
}

function stateMap(s) {
  return s === "OK" ? "online" : s === "STALE" ? "degraded" : "lost";
}

// One line -> typed message, or null for blank input. Unknown prefixes yield { type: "?" }.
export function parseLine(line) {
  const t = String(line ?? "").trim().split(/\s+/);
  if (!t[0]) return null;
  switch (t[0]) {
    case "E":
      return {
        type: "E",
        event: {
          node_id: normalizeNodeId(t[1]),
          ev_seq: num(t[2]),
          master_tick: t[3],
          flags: num(t[4]),
          rssi: num(t[5]),
          snr: num(t[6]),
          master_boot_id: num(t[7]),
          sensor_boot_id: num(t[8]),
          capture_seq: num(t[9]),
          end_seq: num(t[10]),
          end_tick: t[11],
          sync_age_ms: num(t[12]),
        },
      };
    case "D":
      return {
        type: "D",
        telemetry: {
          node_id: normalizeNodeId(t[1]),
          link_state: stateMap(t[2]),
          offset_us: Math.round(num(t[3]) / 16),
          skew_ppm: num(t[4]),
          rx_miss: num(t[5]),
          beacon_gap: num(t[6]),
          last_seen_ms: num(t[7]),
          rssi: num(t[8]),
          snr: num(t[9]),
          latency_ms: num(t[10]),
          temp_c10: num(t[11]),
          batt_mv: num(t[12]),
          sec_drop: num(t[13]),
          provisioned: num(t[14]),
          sync_valid: num(t[15]),
          skew_valid: num(t[16]),
          clock_source: t[17] === "XTAL" ? "xtal" : "rc",
          sync_age_ms: num(t[18]),
          capture_overflow: num(t[19]),
          event_drop: num(t[20]),
          queue_depth: num(t[21]),
          queue_overflow: num(t[22]),
          usb_ref_valid: num(t[23]),
          usb_ref_ppm: num(t[24]),
          sensor_boot_id: num(t[25]),
          master_boot_id: num(t[26]),
        },
      };
    case "H":
      return { type: "H", nowTick: t[1], uptimeMs: num(t[2]), beaconSeq: num(t[3]), nseen: num(t[4]) };
    case "T":
      return { type: "T", requestId: t[1], masterTick: t[2], masterBootId: num(t[3]) };
    case "P":
      return {
        type: "P",
        pps: { tick: t[1], utc: num(t[2]), ppb: num(t[3]), valid: num(t[4]), fix: num(t[5]), sats: num(t[6]), span: num(t[7]) },
      };
    case "I":
      return {
        type: "I",
        product: t[1],
        fw: t[2],
        devid: t[3],
        freqMhz: num(t[4]),
        sf: num(t[5]),
        bw: num(t[6]),
        ticksPerMs: num(t[7]),
      };
    case "A":
      return { type: "A", cmd: t[1] };
    case "X":
      return { type: "X", reason: t[1] };
    default:
      return { type: "?", raw: line };
  }
}

// Event validation, identical to the server ingest contract. Returns the storable row.
export function validateEvent(e) {
  if (!validateNodeId(String(e.node_id))) return { ok: false, reason: "node_id" };
  const tick = tickToText(e.master_tick);
  if (tick === undefined || tick === null) return { ok: false, reason: "master_tick" };
  if (!Number.isInteger(e.ev_seq) || e.ev_seq < 0 || e.ev_seq > 0xffff) return { ok: false, reason: "ev_seq" };
  if (!validBootId(e.master_boot_id)) return { ok: false, reason: "master_boot_id" };
  const endTick = tickToText(e.end_tick);
  if (
    !validBootId(e.sensor_boot_id) ||
    !validBootId(e.capture_seq) ||
    !validBootId(e.end_seq) ||
    endTick == null ||
    !Number.isInteger(e.flags) ||
    e.flags < 0 ||
    e.flags > 127 ||
    !Number.isInteger(e.sync_age_ms) ||
    e.sync_age_ms < 0 ||
    e.sync_age_ms > 65535 ||
    (e.end_seq - e.capture_seq) >>> 0 >= 0x80000000 ||
    (!(e.flags & CAPTURE_LOSS) && (e.end_seq !== e.capture_seq || endTick !== tick)) ||
    (e.flags & CAPTURE_LOSS && e.flags & CAPTURE_CHECKPOINT)
  ) {
    return { ok: false, reason: "capture_evidence" };
  }
  return {
    ok: true,
    row: {
      node_id: String(e.node_id),
      ev_seq: e.ev_seq,
      master_tick: tick,
      flags: e.flags,
      rssi: Number.isFinite(e.rssi) ? e.rssi : null,
      snr: Number.isFinite(e.snr) ? e.snr : null,
      master_boot_id: e.master_boot_id,
      sensor_boot_id: e.sensor_boot_id,
      capture_seq: e.capture_seq,
      end_seq: e.end_seq,
      end_tick: endTick,
      sync_age_ms: e.sync_age_ms,
    },
  };
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

// Telemetry normalisation (server ingest contract). `last_seen_at` is the absolute time the
// master last heard the node: a D line that reports STALE/LOST must not reset it to "now".
export function normalizeTelemetry(t, now = Date.now()) {
  if (!validateNodeId(String(t.node_id))) return null;
  const heardAgeMs = Number.isFinite(t.last_seen_ms) && t.last_seen_ms >= 0 ? Math.trunc(t.last_seen_ms) : null;
  return {
    node_id: String(t.node_id),
    rssi: Number.isFinite(t.rssi) ? t.rssi : null,
    snr: Number.isFinite(t.snr) ? t.snr : null,
    offset_us: intOrNull(t.offset_us),
    skew_ppm: Number.isFinite(t.skew_ppm) ? t.skew_ppm : null,
    latency_ms: Number.isFinite(t.latency_ms) ? t.latency_ms : null,
    rx_miss: intOrNull(t.rx_miss),
    beacon_gap: intOrNull(t.beacon_gap),
    temp_c10: intOrNull(t.temp_c10),
    batt_mv: intOrNull(t.batt_mv),
    sec_drop: intOrNull(t.sec_drop),
    provisioned: bitOrNull(t.provisioned),
    sensor_boot_id: validBootId(t.sensor_boot_id) ? t.sensor_boot_id : null,
    master_boot_id: validBootId(t.master_boot_id) ? t.master_boot_id : null,
    sync_valid: bitOrNull(t.sync_valid),
    skew_valid: bitOrNull(t.skew_valid),
    clock_source: ["xtal", "rc"].includes(t.clock_source) ? t.clock_source : null,
    sync_age_ms: nonNegOrNull(t.sync_age_ms),
    capture_overflow: nonNegOrNull(t.capture_overflow),
    event_drop: nonNegOrNull(t.event_drop),
    queue_depth: nonNegOrNull(t.queue_depth),
    queue_overflow: nonNegOrNull(t.queue_overflow),
    usb_ref_valid: bitOrNull(t.usb_ref_valid),
    usb_ref_ppm: intOrNull(t.usb_ref_ppm),
    link_state: ["online", "degraded", "lost"].includes(t.link_state) ? t.link_state : null,
    last_seen_at: heardAgeMs === null ? now : now - heardAgeMs,
    received_at: now,
  };
}

// GPS/PPS report normalisation. `tick` = master tick of the latest PPS edge (0 when none yet),
// `utc` = Unix seconds of that edge or null, `ppb` = HFXO error (positive = fast), `at` = receive time.
export function normalizePps(p, now = Date.now()) {
  const tick = tickToText(p.tick);
  if (tick == null) return null;
  return {
    tick,
    utc: Number.isInteger(p.utc) && p.utc > 0 ? p.utc : null,
    ppb: Number.isInteger(p.ppb) ? p.ppb : 0,
    valid: bitOrNull(p.valid) ?? 0,
    fix: nonNegOrNull(p.fix) ?? 0,
    sats: nonNegOrNull(p.sats) ?? 0,
    span: nonNegOrNull(p.span) ?? 0,
    at: now,
  };
}

// Dedupe key: the master re-sends the queue head until the host commits it.
export function eventKey(e) {
  return `${e.node_id}:${e.ev_seq}:${e.master_tick}:${e.master_boot_id}:${e.sensor_boot_id}`;
}

// Host commit; the master pops the event only if this matches its queue head exactly.
export function formatAck(e) {
  return `C ${e.node_id} ${e.ev_seq} ${e.master_tick} ${e.master_boot_id} ${e.sensor_boot_id}`;
}

export function isHexKey(s) {
  return typeof s === "string" && /^[0-9a-fA-F]{64}$/.test(s);
}
