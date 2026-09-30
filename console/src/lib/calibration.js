/* PPS timeline: master ticks -> GPS seconds (exact rationals), from the qualified PPS edges
 * of one master boot. DESIGN §8 / PLAN W10.
 *
 * Edges of one segment are exactly one GPS second apart per unit of n. Neighbouring segments
 * are bridged when the whole seconds between them are known (UTC, or tick rounding within an
 * hour) and the ticks across the gap match that many seconds at the neighbours' measured
 * frequencies; otherwise they belong to separate islands. Ticks outside PPS coverage are
 * extrapolated with the nearest segment's measured frequency; islands meet at the midpoint of
 * their gap. A boot without qualified edges is converted at the nominal 16 MHz.
 */
import { MASTER_TICKS_PER_S } from "./event-timing";

const F_NOMINAL = MASTER_TICKS_PER_S;
const WINDOW_S = 64n; // edges used for an extrapolation frequency
const BRIDGE_MAX_TICKS = 3600n * F_NOMINAL;
const BRIDGE_PPM_DIV = 5000n; // 200 ppm, the firmware's gate between qualified PPS edges
const BRIDGE_JITTER_TICKS = 16n; // 1 us: PPS jitter and capture quantization at both ends
const BRIDGE_DRIFT_DIV = 1_000_000n; // 1 ppm of the gap: what the measured frequencies miss

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

const abs = (v) => (v < 0n ? -v : v);
// |r| > t for rationals (denominators are positive).
const exceeds = (r, t) => abs(r.n) * t.d > t.n * r.d;

// Ticks per second over at most WINDOW_S seconds at one end of a segment, with the window's
// middle in seconds from that end edge (<= 0 at the end, >= 0 at the start); null with fewer
// than two edges there.
function endWindow(seg, atEnd) {
  const edges = seg.edges;
  if (atEnd) {
    const last = edges[edges.length - 1];
    let first = last;
    for (let i = edges.length - 2; i >= 0 && last.n - edges[i].n <= WINDOW_S; i--) first = edges[i];
    if (first === last) return null;
    return { f: q(last.tick - first.tick, last.n - first.n), mid: q(first.n - last.n, 2n) };
  }
  const first = edges[0];
  let last = first;
  for (let i = 1; i < edges.length && edges[i].n - first.n <= WINDOW_S; i++) last = edges[i];
  if (last === first) return null;
  return { f: q(last.tick - first.tick, last.n - first.n), mid: q(last.n - first.n, 2n) };
}

// Ticks per second measured over at most WINDOW_S seconds at one end of a segment.
function segmentFrequency(seg, atEnd) {
  return endWindow(seg, atEnd)?.f ?? q(F_NOMINAL);
}

// Frequencies at L (last edge of a) and F (first edge of b), k seconds apart: the two end
// windows' frequencies moved along the trend between their middles, so a crystal drifting
// linearly with temperature is followed. Null unless both ends have a measured frequency:
// one side alone gives no trend.
function gapFrequencies(a, b, k) {
  const wa = endWindow(a, true);
  const wb = endWindow(b, false);
  if (!wa || !wb) return null;
  const trend = div(sub(wb.f, wa.f), sub(add(q(k), wb.mid), wa.mid));
  return { fL: sub(wa.f, mul(trend, wa.mid)), fF: sub(wb.f, mul(trend, wb.mid)) };
}

// Bridge from the last edge of a to the first edge of b: { k whole GPS seconds, gap
// frequencies }, or null. The ticks across the gap must be k seconds at the neighbours'
// frequencies within 1 us + 1 ppm: edges that disagree more (a PPS phase step on
// reacquisition, say) are not bridged, and the gap is extrapolated. Without a measured
// frequency on both sides only the firmware gate's 200 ppm can be checked.
function bridgeOf(a, b) {
  const L = a.edges[a.edges.length - 1];
  const F = b.edges[0];
  const dt = F.tick - L.tick;
  if (dt <= 0n) return null;
  const ua = utcBase(a);
  const ub = utcBase(b);
  let k = null;
  if (ua != null && ub != null) {
    const byUtc = ub + F.n - (ua + L.n);
    if (byUtc >= 1n && abs(byUtc * F_NOMINAL - dt) <= F_NOMINAL / 2n + dt / 1000n) k = byUtc;
  }
  if (k == null && dt <= BRIDGE_MAX_TICKS) {
    const rounded = roundHalfUp(q(dt, F_NOMINAL));
    if (rounded >= 1n) k = rounded;
  }
  if (k == null) return null;
  const freq = gapFrequencies(a, b, k);
  if (!freq) return abs(k * F_NOMINAL - dt) > (k * F_NOMINAL) / BRIDGE_PPM_DIV ? null : { k, freq: null };
  const expected = mul(q(k), div(add(freq.fL, freq.fF), q(2n)));
  const tolerance = q(BRIDGE_JITTER_TICKS + (k * F_NOMINAL) / BRIDGE_DRIFT_DIV);
  return exceeds(sub(q(dt), expected), tolerance) ? null : { k, freq };
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
    const bridge = bridgeOf(prev, seg);
    if (bridge) {
      seg.island = island;
      seg.base = sub(add(tL, q(bridge.k)), q(F.n));
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

// T(tick) in GPS seconds, with how it was obtained and the island it was taken from.
export function timeAt(timeline, tickValue) {
  const tick = toBig(tickValue);
  const edges = timeline.edges;
  if (!edges.length) return { t: q(tick, F_NOMINAL), how: "nominal", extrapTicks: null, island: null };
  const i = lastAtOrBefore(edges, tick);
  if (i >= 0 && edges[i].tick === tick) return { t: edges[i].t, how: "interp", extrapTicks: null, island: edges[i].island };
  const forward = (e) => ({ t: add(e.t, div(q(tick - e.tick), segmentFrequency(timeline.segs[e.seg], true))), how: "extrap", extrapTicks: tick - e.tick, island: e.island });
  const backward = (e) => ({ t: sub(e.t, div(q(e.tick - tick), segmentFrequency(timeline.segs[e.seg], false))), how: "extrap", extrapTicks: e.tick - tick, island: e.island });
  if (i < 0) return backward(edges[0]);
  if (i === edges.length - 1) return forward(edges[i]);
  const a = edges[i];
  const b = edges[i + 1];
  if (a.island === b.island) {
    const t = add(a.t, div(mul(q(tick - a.tick), sub(b.t, a.t)), q(b.tick - a.tick)));
    return { t, how: a.seg === b.seg ? "interp" : "bridge", extrapTicks: null, island: a.island };
  }
  return 2n * tick <= a.tick + b.tick ? forward(a) : backward(b);
}

function toPoint({ t, how, extrapTicks, island }) {
  return { num: String(t.n), den: String(t.d), how, extrapTicks: extrapTicks == null ? null : String(extrapTicks), island };
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
  if (list.some((p) => p.how === "extrap")) return "gps-extrapolated";
  // Two islands are joined only by extrapolation across the gap between them.
  const islands = new Set(list.map((p) => p.island).filter((v) => v != null));
  return islands.size > 1 ? "gps-extrapolated" : "gps";
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
