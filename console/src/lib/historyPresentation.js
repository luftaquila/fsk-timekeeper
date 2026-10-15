import { MODE_LABEL, RESULT_CLASSIFICATIONS } from "./constants";
import { formatDuration, fmtDateTime } from "./format";
import { resultNs, lapsNs } from "./results";

export function statusValue(row) {
  if (RESULT_CLASSIFICATIONS.includes(row.classification)) return row.classification;
  return row.verification === "dnf" ? row.dnfReason || "DNF" : row.verification;
}

export function statusLabel(row) {
  const value = statusValue(row);
  return ({ verified: "Verified", invalid: "Invalid", pending: "Pending" })[value] || value;
}

export function historyRecord(row, edges) {
  return {
    Date: fmtDateTime(row.createdAt),
    Mode: MODE_LABEL[row.mode] || row.mode,
    Note: row.note || "-",
    Result: formatDuration(resultNs(row, edges)),
    Status: [statusLabel(row), ...(row.durable === false ? ["Not Saved"] : [])].join("\n"),
  };
}

export function lapSummary(row, edges) {
  if (row.mode !== "laps") return null;
  const laps = lapsNs(row, edges);
  if (!laps.length) return null;
  const best = laps.reduce((a, b) => b < a ? b : a);
  return { laps, best, bestIndex: laps.indexOf(best), total: laps.reduce((sum, ns) => sum + ns, 0n) };
}

export { formatDelta as lapDelta } from "./format";
