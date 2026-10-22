/* Line-scan camera crossings (camera node rows carry `camera`: { line, period, run_lo, run_hi, car,
 * conf }; lines run_lo..run_hi expose at master_tick + (n - line) * period). Operator decisions
 * never edit evidence; a run keeps them by crossing key:
 *   decisions: { [key]: { line?: string, car?: string | null, ignore?: true, at } }
 * line: the front line the operator picked (its tick from the crossing's own run of lines);
 * car: the car number (camera-laps assignment, else display only; null = unassigned);
 * ignore: not a vehicle, never a crossing. */
import { CAR_NUMBER_PATTERN } from "./constants";

export const crossingKey = (row) => `${row.node_id}:${row.sensor_boot_id}:${row.capture_seq}`;

export function carText(n) {
  return Number.isInteger(n) && n > 0 && n < 1000 ? String(n) : null;
}

export function isCarNumber(s) {
  return typeof s === "string" && CAR_NUMBER_PATTERN.test(s);
}

// A typed car number as kept: digits without leading zeros ("007" -> "7"), anything else as typed.
export function carNumberOf(text) {
  const s = String(text ?? "").trim();
  return /^\d{1,3}$/.test(s) ? String(Number(s)) : s;
}

// Lines whose tick the crossing's record fixes: the operator may pick a front line here.
export function pickableLines(camera) {
  return { first: BigInt(camera.run_lo) + 1n, last: BigInt(camera.run_hi) };
}

// Exposure tick (bigint) of line within the crossing's run, else null.
export function lineTick(row, line) {
  const c = row?.camera;
  if (!c) return null;
  const n = BigInt(line);
  const { first, last } = pickableLines(c);
  if (n < first || n > last) return null;
  return BigInt(row.master_tick) + (n - BigInt(c.line)) * BigInt(c.period);
}

// The crossing tick after the operator's line decision (bigint).
export function decidedTick(row, decision) {
  if (decision?.line != null) {
    const t = lineTick(row, decision.line);
    if (t != null) return t;
  }
  return BigInt(row.master_tick);
}

// OCR car number when confident enough, else null (unassigned).
export function autoCar(row, run) {
  const car = carText(row.camera?.car);
  return car && row.camera.conf >= (run.ocrMinConf ?? 0) ? car : null;
}

// The camera Pi's line timing (/api/config lineTiming): the master trigger period a line rate runs
// at, rounded as the Pi rounds it (Python round(): a half goes to the even neighbour), and the
// longest exposure that leaves the camera its readout within that period (config.exposure_max_us).
export function linePeriod(rateHz, t) {
  const x = t.masterHz / rateHz;
  const whole = Math.floor(x);
  const up = x - whole > 0.5 || (x - whole === 0.5 && whole % 2 === 1);
  return Math.max(t.periodMin, Math.min(t.periodMax, up ? whole + 1 : whole));
}

export const lineRateAt = (rateHz, t) => t.masterHz / linePeriod(rateHz, t);

export function exposureMaxUs(rateHz, t) {
  const ticks = Math.floor((linePeriod(rateHz, t) * t.cameraHz) / t.masterHz) - t.readoutTicks;
  return (ticks * 1e6) / t.cameraHz;
}

export function decidedCar(row, decision, run) {
  if (decision && "car" in decision) return decision.car;
  return autoCar(row, run);
}

// A sensor stream with the operator's decisions applied to its camera edges.
export function decideStream(stream, decisions = {}) {
  const edges = [];
  for (const edge of stream.edges) {
    if (!edge.row.camera) {
      edges.push(edge);
      continue;
    }
    const decision = decisions[crossingKey(edge.row)];
    if (decision?.ignore) continue;
    edges.push({ ...edge, tick: decidedTick(edge.row, decision) });
  }
  return { ...stream, edges };
}

// A decision patch checked against its crossing row; returns the merged decision.
export function mergeDecision(row, previous = {}, patch = {}, now = Date.now()) {
  if (!row?.camera) throw new Error("Not a camera crossing.");
  const next = { ...previous };
  if ("line" in patch) {
    if (patch.line == null) delete next.line;
    else if (lineTick(row, patch.line) == null) throw new Error("That line has no exact time.");
    else next.line = String(BigInt(patch.line));
  }
  if ("car" in patch) {
    if (patch.car !== null && !isCarNumber(patch.car)) throw new Error("Car numbers have one to three digits.");
    next.car = patch.car;
  }
  if ("ignore" in patch) {
    if (patch.ignore) next.ignore = true;
    else delete next.ignore;
  }
  next.at = now;
  return next;
}
