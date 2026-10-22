// Result durations from raw ticks: frozen calibration, else provisional from PPS edges at hand.
import { calibrationPoints, durationNs, pointsMethod } from "./calibration";
import { MODE_KIND } from "./constants";

// One car of a camera-laps heat as a laps run (its own ticks, state and frozen calibration).
export function carRun(run, car) {
  const r = run.results?.[car];
  if (!r) return null;
  return { ...run, mode: "laps", car, crossingTicks: r.crossingTicks, heldTicks: r.heldTicks || [], verification: r.verification,
    classification: r.classification, calibration: r.calibration, durationNs: r.durationNs, fault: r.fault,
    recordedAt: r.recordedAt, closed: r.closed };
}

// Ticks the result depends on.
export function resultTicks(run) {
  if (MODE_KIND[run.mode] === "laps") return run.crossingTicks || [];
  return [run.startTick, run.finishTick].filter((t) => t != null);
}

// source: a buildTimeline() result or an edge list (the provisional calibration).
function pointsFor(run, ticks, source) {
  const frozen = run.calibration?.points;
  if (frozen && ticks.every((t) => frozen[String(t)])) return frozen;
  return calibrationPoints(source, ticks);
}

// Duration (ns, bigint) between two ticks of the run.
export function spanNs(run, a, b, source = null) {
  return durationNs(pointsFor(run, [a, b], source), a, b);
}

// The result: sprint F1 − S1; laps last − first crossing, void once a loss made it invalid and
// held while an unassigned crossing may be the car's (heldTicks).
export function resultNs(run, source = null) {
  if (!run) return null;
  if (run.durationNs != null) return BigInt(run.durationNs);
  if (MODE_KIND[run.mode] === "sprint") return run.startTick && run.finishTick ? spanNs(run, run.startTick, run.finishTick, source) : null;
  const c = run.crossingTicks || [];
  if (run.verification === "invalid" || run.heldTicks?.length || c.length < 2) return null;
  return spanNs(run, c[0], c[c.length - 1], source);
}

// The stored exact result (durationNs): the current result under the frozen calibration only.
export function frozenDurationNs(run) {
  const frozen = run.calibration?.points;
  if (!frozen || !resultTicks(run).every((t) => frozen[t])) return null;
  const ns = resultNs({ ...run, durationNs: null });
  return ns == null ? null : String(ns);
}

// Laps (ns, bigint) of a laps run; for a run invalid by a loss, the laps before it.
export function lapsNs(run, source = null) {
  if (!run) return [];
  const c = run.crossingTicks || [];
  if (c.length < 2) return [];
  const points = pointsFor(run, c, source);
  const out = [];
  for (let i = 1; i < c.length; i++) out.push(durationNs(points, c[i - 1], c[i]));
  return out;
}

// Indexes of the laps an unassigned crossing falls in (heldTicks): each may be two laps.
export function heldLaps(run) {
  const c = (run?.crossingTicks || []).map(BigInt);
  const held = (run?.heldTicks || []).map(BigInt);
  const out = new Set();
  for (let i = 1; i < c.length; i++) if (held.some((t) => t > c[i - 1] && t < c[i])) out.add(i - 1);
  return out;
}

// Whether the result's ticks are converted with GPS PPS rather than nominal 16 MHz (frozen
// calibration, else the edges at hand); null without result ticks.
export function resultUsesPps(run, source = null) {
  const ticks = resultTicks(run);
  return ticks.length ? pointsMethod(pointsFor(run, ticks, source), ticks) !== "nominal" : null;
}
