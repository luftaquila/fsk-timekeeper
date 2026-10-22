import { MODE_LABEL, MODE_KIND, RESULT_CLASSIFICATIONS } from "./constants";
import { formatDuration, fmtDateTime } from "./format";
import { resultNs, lapsNs, heldLaps, resultUsesPps } from "./results";

export function statusValue(row) {
  if (RESULT_CLASSIFICATIONS.includes(row.classification)) return row.classification;
  return row.verification;
}

export function statusLabel(row) {
  const value = statusValue(row);
  return ({ verified: "OK", invalid: "Invalid", pending: "Pending" })[value] || value;
}

// What converted the result's ticks: GPS PPS, or nothing (nominal 16 MHz).
export function calibrationLabel(row, edges) {
  const pps = resultUsesPps(row, edges);
  return pps == null ? "-" : pps ? "GPS" : "None";
}

export function historyRecord(row, edges) {
  return {
    Date: fmtDateTime(row.startedAt),
    Mode: MODE_LABEL[row.mode] || row.mode,
    Note: row.note || "-",
    Result: formatDuration(resultNs(row, edges)),
    Status: [statusLabel(row), ...(row.durable === false ? ["Not Saved"] : [])].join("\n"),
    Calibration: calibrationLabel(row, edges),
  };
}

// held: laps an unassigned crossing falls in (shown as ?); while one may be the car's, its best
// lap and average are unknown too (best null).
export function lapSummary(row, edges) {
  if (MODE_KIND[row.mode] !== "laps") return null;
  const laps = lapsNs(row, edges);
  if (!laps.length) return null;
  const held = heldLaps(row);
  const best = row.heldTicks?.length ? null : laps.reduce((a, b) => b < a ? b : a);
  // total is the run's result, blank once a loss voids it; sum only feeds the average.
  return { laps, held, best, bestIndex: best == null ? -1 : laps.indexOf(best), sum: laps.reduce((sum, ns) => sum + ns, 0n), total: resultNs(row, edges) };
}

export { formatDelta as lapDelta } from "./format";
