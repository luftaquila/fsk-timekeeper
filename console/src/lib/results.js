/* Result durations of a run or history row, from its raw ticks and calibration.
 * The frozen calibration wins; before it exists the PPS edges at hand give a provisional value.
 * Rows of run schema < 3 kept only rounded ms (`result`, `laps`); they are read as is. */
import { calibrationPoints, durationNs, pointsMethod, CALIBRATION_LABEL } from "./calibration";

export function isOldFormatRow(row) {
  return !!row && row.schema == null;
}

// Ticks the result depends on.
export function resultTicks(run) {
  if (run.mode === "laps") return run.crossingTicks || [];
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

// The result: sprint F1 − S1, laps last − first confirmed crossing (only while the total is valid).
export function resultNs(run, source = null) {
  if (!run) return null;
  if (isOldFormatRow(run)) return Number.isFinite(run.result) ? BigInt(Math.round(run.result)) * 1_000_000n : null;
  if (run.durationNs != null) return BigInt(run.durationNs);
  if (run.mode === "sprint") return run.startTick && run.finishTick ? spanNs(run, run.startTick, run.finishTick, source) : null;
  const c = run.crossingTicks || [];
  if (!run.totalValid || c.length < 2) return null;
  return spanNs(run, c[0], c[c.length - 1], source);
}

// Confirmed laps (ns, bigint) of a laps run; the confirmed ones only, also for an invalid run.
export function lapsNs(run, source = null) {
  if (!run) return [];
  if (isOldFormatRow(run)) return (run.laps || []).filter(Number.isFinite).map((ms) => BigInt(Math.round(ms)) * 1_000_000n);
  const c = run.crossingTicks || [];
  if (c.length < 2) return [];
  const points = pointsFor(run, c, source);
  const out = [];
  for (let i = 1; i < c.length; i++) out.push(durationNs(points, c[i - 1], c[i]));
  return out;
}

// "gps" | "gps-extrapolated" | "nominal" | null (old-format rows / nothing to convert).
export function calibrationMethod(run, source = null) {
  if (!run || isOldFormatRow(run)) return null;
  if (run.calibration?.method) return run.calibration.method;
  const ticks = resultTicks(run);
  return ticks.length ? pointsMethod(calibrationPoints(source, ticks)) : null;
}

export function calibrationLabel(method) {
  return method ? CALIBRATION_LABEL[method] || method : "";
}
