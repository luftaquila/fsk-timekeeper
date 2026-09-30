/* PPS timeline: master ticks -> GPS seconds (exact rationals), from the qualified PPS edges
 * of one master boot. DESIGN §8 / PLAN W10.
 *
 * Edges of one segment are exactly one GPS second apart per unit of n. Neighbouring segments
 * are bridged when the whole seconds between them are known (UTC, or tick rounding within an
 * hour); otherwise they belong to separate islands. Ticks outside PPS coverage are
 * extrapolated with the nearest segment's measured frequency; islands meet at the midpoint of
 * their gap. A boot without qualified edges is converted at the nominal 16 MHz.
 */
import { MASTER_TICKS_PER_S } from "./event-timing";

const F_NOMINAL = MASTER_TICKS_PER_S;
const WINDOW_S = 64n; // edges used for an extrapolation frequency
const BRIDGE_MAX_TICKS = 3600n * F_NOMINAL;

function gcd(a, b) {
  if (a < 0n) a = -a;
  if (b < 0n) b = -b;
  while (b) [a, b] = [b, a % b];
  return a;
}
function q(n, d = 1n) {
  if (d < 0n) {
    n = -n;
    d = -d;
  }
  const g = gcd(n, d) || 1n;
  return { n: n / g, d: d / g };
}
const add = (a, b) => q(a.n * b.d + b.n * a.d, a.d * b.d);
const sub = (a, b) => q(a.n * b.d - b.n * a.d, a.d * b.d);
const mul = (a, b) => q(a.n * b.n, a.d * b.d);
const div = (a, b) => q(a.n * b.d, a.d * b.n);
function floorDiv(n, d) {
  const r = n / d;
  return n % d !== 0n && n < 0n !== d < 0n ? r - 1n : r;
}
// Nearest integer, halves rounded up.
function roundHalfUp(r) {
  return floorDiv(2n * r.n + r.d, 2n * r.d);
}

function toBig(v) {
  return typeof v === "bigint" ? v : BigInt(v);
}

function normalizeEdges(edges) {
  const byTick = new Map();
  for (const e of edges || []) {
    if (!e || !(Number(e.seg) >= 1) || !Number.isInteger(Number(e.n)) || e.tick == null) continue;
    const tick = toBig(e.tick);
    if (!byTick.has(tick)) byTick.set(tick, { tick, seg: Number(e.seg), n: BigInt(e.n), utc: Number.isInteger(e.utc) && e.utc > 0 ? BigInt(e.utc) : null });
  }
  return [...byTick.values()].sort((a, b) => (a.tick < b.tick ? -1 : 1));
}

// UTC second of n = 0 in a segment, from any edge that carries UTC.
function utcBase(seg) {
  for (const e of seg.edges) if (e.utc != null) return e.utc - e.n;
  return null;
}

// Whole GPS seconds between the last edge of a and the first edge of b, or null.
function bridgeSeconds(a, b) {
  const L = a.edges[a.edges.length - 1];
  const F = b.edges[0];
  const dt = F.tick - L.tick;
  if (dt <= 0n) return null;
  const ua = utcBase(a);
  const ub = utcBase(b);
  if (ua != null && ub != null) {
    const k = ub + F.n - (ua + L.n);
    const err = k * F_NOMINAL - dt;
    if (k >= 1n && (err < 0n ? -err : err) <= F_NOMINAL / 2n + dt / 1000n) return k;
  }
  if (dt <= BRIDGE_MAX_TICKS) {
    const k = roundHalfUp(q(dt, F_NOMINAL));
    return k >= 1n ? k : null;
  }
  return null;
}

// Ticks per second measured over at most WINDOW_S seconds at one end of a segment.
function segmentFrequency(seg, atEnd) {
  const edges = seg.edges;
  if (edges.length < 2) return q(F_NOMINAL);
  if (atEnd) {
    const last = edges[edges.length - 1];
    let first = last;
    for (let i = edges.length - 2; i >= 0 && last.n - edges[i].n <= WINDOW_S; i--) first = edges[i];
    return first === last ? q(F_NOMINAL) : q(last.tick - first.tick, last.n - first.n);
  }
  const first = edges[0];
  let last = first;
  for (let i = 1; i < edges.length && edges[i].n - first.n <= WINDOW_S; i++) last = edges[i];
  return last === first ? q(F_NOMINAL) : q(last.tick - first.tick, last.n - first.n);
}

export function buildTimeline(edges) {
  const list = normalizeEdges(edges);
  const segs = [];
  for (const e of list) {
    const seg = segs[segs.length - 1];
    if (seg && seg.id === e.seg && e.n > seg.edges[seg.edges.length - 1].n) seg.edges.push(e);
    else segs.push({ id: e.seg, edges: [e], base: null, island: 0 });
  }
  let island = 0;
  for (let i = 0; i < segs.length; i++) {
    const seg = segs[i];
    if (i === 0) {
      seg.base = q(-seg.edges[0].n);
      continue;
    }
    const prev = segs[i - 1];
    const L = prev.edges[prev.edges.length - 1];
    const F = seg.edges[0];
    const tL = add(prev.base, q(L.n));
    const k = bridgeSeconds(prev, seg);
    if (k != null) {
      seg.island = island;
      seg.base = sub(add(tL, q(k)), q(F.n));
      continue;
    }
    // New island: each half of the gap is extrapolated from its own side; T meets at the midpoint.
    island += 1;
    seg.island = island;
    const mid = q(L.tick + F.tick, 2n);
    const tMid = add(tL, div(sub(mid, q(L.tick)), segmentFrequency(prev, true)));
    const tF = add(tMid, div(sub(q(F.tick), mid), segmentFrequency(seg, false)));
    seg.base = sub(tF, q(F.n));
  }
  const flat = [];
  segs.forEach((seg, index) => {
    for (const e of seg.edges) flat.push({ tick: e.tick, t: add(seg.base, q(e.n)), seg: index, island: seg.island });
  });
  return { segs, edges: flat };
}

function lastAtOrBefore(edges, tick) {
  let lo = 0;
  let hi = edges.length - 1;
  let found = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (edges[mid].tick <= tick) {
      found = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  return found;
}

// T(tick) in GPS seconds, with how it was obtained.
export function timeAt(timeline, tickValue) {
  const tick = toBig(tickValue);
  const edges = timeline.edges;
  if (!edges.length) return { t: q(tick, F_NOMINAL), how: "nominal", extrapTicks: null };
  const i = lastAtOrBefore(edges, tick);
  if (i >= 0 && edges[i].tick === tick) return { t: edges[i].t, how: "interp", extrapTicks: null };
  const forward = (e) => ({ t: add(e.t, div(q(tick - e.tick), segmentFrequency(timeline.segs[e.seg], true))), how: "extrap", extrapTicks: tick - e.tick });
  const backward = (e) => ({ t: sub(e.t, div(q(e.tick - tick), segmentFrequency(timeline.segs[e.seg], false))), how: "extrap", extrapTicks: e.tick - tick });
  if (i < 0) return backward(edges[0]);
  if (i === edges.length - 1) return forward(edges[i]);
  const a = edges[i];
  const b = edges[i + 1];
  if (a.island === b.island) {
    const t = add(a.t, div(mul(q(tick - a.tick), sub(b.t, a.t)), q(b.tick - a.tick)));
    return { t, how: a.seg === b.seg ? "interp" : "bridge", extrapTicks: null };
  }
  return 2n * tick <= a.tick + b.tick ? forward(a) : backward(b);
}

function toPoint({ t, how, extrapTicks }) {
  return { num: String(t.n), den: String(t.d), how, extrapTicks: extrapTicks == null ? null : String(extrapTicks) };
}

function pointTime(point) {
  return q(BigInt(point.num), BigInt(point.den));
}

function asTimeline(source) {
  return Array.isArray(source) || source == null ? buildTimeline(source || []) : source;
}

// Point records for every tick (decimal-string keys) from one timeline snapshot.
// source: a buildTimeline() result or the edge list itself.
export function calibrationPoints(source, ticks) {
  const timeline = asTimeline(source);
  const points = {};
  for (const tick of ticks || []) {
    if (tick == null) continue;
    const key = String(tick);
    if (!points[key]) points[key] = toPoint(timeAt(timeline, key));
  }
  return points;
}

export function pointsMethod(points, ticks = null) {
  const list = (ticks ? ticks.map((t) => points?.[String(t)]) : Object.values(points || {})).filter(Boolean);
  if (!list.length || list.some((p) => p.how === "nominal")) return "nominal";
  return list.some((p) => p.how === "extrap") ? "gps-extrapolated" : "gps";
}

// (T(b) − T(a)) in ns, rounded half up; null when a point is missing.
export function durationNs(points, a, b) {
  const pa = points?.[String(a)];
  const pb = points?.[String(b)];
  if (!pa || !pb) return null;
  return roundHalfUp(mul(sub(pointTime(pb), pointTime(pa)), q(1_000_000_000n)));
}

export function freezeCalibration(source, ticks, now = Date.now()) {
  const points = calibrationPoints(source, ticks);
  return { version: 1, method: pointsMethod(points), frozenAt: now, points };
}

// Wall-clock UTC (ms) of a tick from the latest qualified edge that carries UTC; null without one.
export function utcMsAt(edges, tickValue) {
  const anchor = [...normalizeEdges(edges)].reverse().find((e) => e.utc != null);
  if (!anchor) return null;
  const timeline = buildTimeline(edges);
  const at = timeAt(timeline, tickValue).t;
  const ref = timeAt(timeline, anchor.tick).t;
  const ms = add(q(anchor.utc * 1000n), mul(sub(at, ref), q(1000n)));
  return Number(roundHalfUp(ms));
}

export const CALIBRATION_LABEL = {
  gps: "GPS",
  "gps-extrapolated": "GPS (extrapolated)",
  nominal: "nominal 16 MHz",
};
