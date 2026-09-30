/* One sensor's evidence timeline for a run.
 *
 * capture_seq order is capture-time order. Walking the stream from the START checkpoint,
 * `known` is the tick through which every edge of this sensor is identified (delivered or
 * covered by a loss record). A hole {lo, hi} says "unreported edges may exist at ticks t
 * with lo < t <= hi"; hi is null while its bound is not known yet, Infinity when open-ended.
 * Holes never stop the walk; missing records do.
 */
import { FLAG_HEALTHY, FLAG_TIME_UNKNOWN, NODE_MASTER } from "./protocol";

const HALF = 0x80000000;
const KIND_ORDER = { capture: 0, loss: 1, checkpoint: 2 };

export const HOLE_REASON = {
  loss: "lost captures",
  unknownLoss: "lost a capture at an unknown time",
  unhealthy: "captured an edge without a valid clock",
  order: "reported capture times out of order",
  quarantine: "sent an event the console quarantined",
  reboot: "rebooted during the run",
};

const distance = (seq, base) => (seq - base) >>> 0;
const tickOf = (row, field = "master_tick") => BigInt(row[field]);
const healthy = (row) => (row.flags & FLAG_HEALTHY) === FLAG_HEALTHY && !(row.flags & FLAG_TIME_UNKNOWN);
const max = (a, b) => (a == null ? b : b == null ? a : a > b ? a : b);

// First timebase-end record of the run's master session at or after the boundary.
export function masterTimebaseEnd(run, rows) {
  const boundary = BigInt(run.boundaryTick);
  let end = null;
  for (const row of rows) {
    if (row.node_id !== NODE_MASTER || row.kind !== "loss" || row.master_boot_id !== run.masterBootId) continue;
    const t = tickOf(row);
    if (t >= boundary && (end == null || t < end)) end = t;
  }
  return end;
}

// -> { node_id, role, known, edges: [{tick, row}], holes: [{lo, hi, reason}] }
export function sensorStream(run, node, rows, { sessionEnd = null } = {}) {
  const src = run.nodes[node];
  const boundary = BigInt(run.boundaryTick);
  const holes = [];
  const edges = [];
  const mine = rows.filter((row) => row.node_id === node && row.master_boot_id === run.masterBootId);
  const markers = rows
    .filter((row) => row.kind === "quarantine" && (row.node_id === node || row.node_id === "*") && (row.master_boot_id == null || row.master_boot_id === run.masterBootId))
    .map((row) => row.seq);
  const current = mine
    .filter((row) => row.sensor_boot_id === src.boot && KIND_ORDER[row.kind] != null && distance(row.capture_seq, src.seq) < HALF)
    .sort(
      (a, b) =>
        distance(a.capture_seq, src.seq) - distance(b.capture_seq, src.seq) || KIND_ORDER[a.kind] - KIND_ORDER[b.kind] || a.seq - b.seq,
    );
  const delivered = new Set(current.filter((row) => row.kind === "capture" && healthy(row)).map((row) => distance(row.capture_seq, src.seq)));

  let next = 1;
  let known = boundary - 1n;
  let last = null;
  let pend = null;
  let lastSeq = src.cpSeq ?? run.cursor ?? 0;
  const openPend = (reason) => {
    if (!pend) pend = { lo: last ?? boundary - 1n, reason };
  };
  const closePend = (hi) => {
    if (pend) holes.push({ lo: pend.lo, hi, reason: pend.reason });
    pend = null;
  };
  const orderFault = (t) => {
    const lo = (t < last ? t : last) - 1n;
    holes.push({ lo: pend && pend.lo < lo ? pend.lo : lo, hi: Infinity, reason: HOLE_REASON.order });
    pend = null;
  };

  for (const row of current) {
    const d = distance(row.capture_seq, src.seq);
    const isCheckpoint = row.kind === "checkpoint";
    if (isCheckpoint ? d > next - 1 : d > next) {
      // Missing records: only a quarantined event in between can explain them.
      if (!markers.some((seq) => seq > lastSeq && seq < row.seq)) break;
      openPend(HOLE_REASON.quarantine);
      next = isCheckpoint ? d + 1 : d;
    }
    if (row.kind === "capture") {
      if (d < next) continue;
      if (!healthy(row)) {
        openPend(HOLE_REASON.unhealthy);
      } else {
        const t = tickOf(row);
        if (last != null && t <= last) {
          orderFault(t);
          break;
        }
        closePend(t);
        if (t >= boundary) edges.push({ tick: t, row });
        known = max(known, t);
        last = t;
      }
      next = d + 1;
      lastSeq = row.seq;
    } else if (row.kind === "loss") {
      const end = distance(row.end_seq, src.seq);
      if (end < next) continue;
      let covered = end - Math.max(d, next) + 1 <= delivered.size;
      for (let k = Math.max(d, next); covered && k <= end; k++) covered = delivered.has(k);
      if (covered) continue;
      if (row.flags & FLAG_TIME_UNKNOWN) {
        openPend(HOLE_REASON.unknownLoss);
      } else {
        const t = tickOf(row);
        const te = tickOf(row, "end_tick");
        closePend(t);
        holes.push({ lo: last != null && last > t - 1n ? last : t - 1n, hi: te, reason: HOLE_REASON.loss });
        known = max(known, te);
        last = max(last, te);
      }
      next = end + 1;
      lastSeq = row.seq;
    } else {
      if (d !== next - 1 || !healthy(row)) continue;
      const t = tickOf(row);
      if (last != null && t < last) {
        orderFault(t);
        break;
      }
      closePend(t);
      known = max(known, t);
      last = max(last, t);
      lastSeq = row.seq;
    }
  }
  if (pend) holes.push({ lo: pend.lo, hi: null, reason: pend.reason });

  const rebooted = mine.some(
    (row) => row.sensor_boot_id !== src.boot && KIND_ORDER[row.kind] != null && !(row.flags & FLAG_TIME_UNKNOWN) && tickOf(row) >= boundary,
  );
  if (rebooted) holes.push({ lo: known, hi: Infinity, reason: HOLE_REASON.reboot });
  if (sessionEnd) holes.push({ lo: known, hi: Infinity, reason: sessionEnd, master: true });
  return { node_id: node, role: src.role, known, edges, holes };
}

/* Per-sensor debounce (leading edge, against the last accepted crossing) and hole
 * significance above `after` (exclusive). A hole wholly inside [c, c + D) of the last
 * accepted crossing c is harmless — its edges would be debounced anyway. The first other
 * hole cuts the list: crossings above cut are uncertain.
 * -> { accepted: bigint[], cut, final, cutHole, certain }
 */
export function acceptCrossings(stream, { after, debounceTicks }) {
  const items = [
    ...stream.edges.map((e) => ({ at: e.tick, order: 0, edge: e })),
    ...stream.holes.map((h) => ({ at: h.lo, order: 1, hole: h })),
  ].sort((a, b) => (a.at < b.at ? -1 : a.at > b.at ? 1 : a.order - b.order));
  const accepted = [];
  let c = null;
  let cut = null;
  let final = false;
  let cutHole = null;
  for (const item of items) {
    if (item.edge) {
      const t = item.edge.tick;
      if (t <= after) continue;
      if (c == null || t - c >= debounceTicks) {
        accepted.push(t);
        c = t;
      }
      continue;
    }
    const { lo, hi } = item.hole;
    if (typeof hi === "bigint" && hi <= after) continue;
    const from = lo > after ? lo : after;
    if (typeof hi === "bigint" && c != null && c <= from && hi - c < debounceTicks) continue;
    cut = from;
    final = hi !== null;
    cutHole = item.hole;
    break;
  }
  const certain = cut != null && cut < stream.known ? cut : stream.known;
  return { accepted, cut, final, cutHole, certain };
}
