/* Run engine: START snapshot, evidence evaluation and run states. A run is plain JSON and is
 * also its history record (no separate result copy):
 * {
 *   schema, radioProto, usbProto, runId, mode: "sprint" | "laps", note,
 *   boundaryTick: string, masterBootId: string,
 *   nodes: { [node_id]: { boot, seq, role, cpSeq } },  // START checkpoint per mapped sensor, one per role
 *   cursor: number,                                    // evidence = event-log rows with seq > cursor
 *   lapTarget: number|null, debounceMs: number,        // frozen at START
 *   armed, closed,
 *   stoppedClockDisplay?: string,                      // timer snapshot at manual Stop, not the result
 *   verification: "pending" | "verified" | "invalid",   // the latest evaluation, also once recorded
 *   classification: "DNS" | "DNF" | "DSQ" | null,      // explicit operator decision
 *   startTick, finishTick: string|null,                // sprint result S1, F1
 *   crossingTicks: string[],                           // laps: crossings C0..Ck
 *   calibration: null | frozen descriptor (calibration.js), durationNs: string|null,
 *   fault: null | { fault_id, mode, run_id, kind, occurred_at, reasons: [{ node_id, role, reason, lo, hi }] },
 *   masterSessionEnded: null | { reason },             // master reboot / version change seen while open
 *   gpsAtStart, startedUtc, startedAt, durable,
 *   recordedAt: number|null,                           // wall ms the result was first decided
 *   masterDevId: string|null,                          // master chip id at START
 *   throughSeq: number,                                // last event-log seq evaluated
 *   imported?, evidenceSeqs?,                          // backup import: flag, remapped evidence seqs
 * }
 * A crossing counts as soon as it is received; nothing waits for another report, since its
 * sensor's earlier records are all in by then. A hole that may hide a crossing the result needs
 * makes the run invalid once its end is known. A capture out of order opens such a hole below the
 * crossings already taken and withdraws them.
 * Camera modes add `decisions` (camera.js): the operator's front lines, car numbers and ignored
 * detections. camera-sprint follows the sprint rules with the camera on the finish. camera-laps is
 * one heat of every car that crosses:
 *   ocrMinConf,                                        // frozen at START
 *   results: { [car]: { crossingTicks, heldTicks, verification, classification, closed, recordedAt,
 *                       calibration, durationNs, fault } }  // a car per number crossings went to
 * Its crossings go to a car (OCR or operator), are ignored, or stay unassigned (evaluateHeat).
 */
import { sensorStream, acceptCrossings, masterTimebaseEnd } from "./sensor-stream";
import { crossingKey, decideStream, decidedTick, decidedCar } from "./camera";
import { FLAG_HEALTHY, FLAG_TIME_UNKNOWN, RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "./protocol";
import { MASTER_TICKS_PER_MS } from "./event-timing";
import { WIRELESS_STATUS_MAX_AGE_MS, DEFAULT_DEBOUNCE_MS, RESULT_CLASSIFICATIONS, MODES, MODE_KIND, DEFAULT_OCR_MIN_CONF } from "./constants";

export const RUN_SCHEMA = 4;
const MASTER_TIMEBASE_ENDED = "The master timebase ended during the run.";
export const MASTER_REBOOTED = "The master rebooted during the run.";

export class EngineError extends Error {
  constructor(code, message, node = null) {
    super(message);
    this.code = code;
    this.node = node;
  }
}

function uuid() {
  return globalThis.crypto?.randomUUID ? globalThis.crypto.randomUUID() : `${Date.now()}-${Math.random()}`;
}

const healthyCheckpoint = (row) =>
  row?.kind === "checkpoint" && (row.flags & FLAG_HEALTHY) === FLAG_HEALTHY && !(row.flags & FLAG_TIME_UNKNOWN);

// START. Every mapped sensor needs a healthy checkpoint of its current boot, received within
// statusMaxAgeMs, at or before the boundary tick; the evidence window opens at the oldest one.
export function createRun({
  mode,
  note = "",
  runId = uuid(),
  clock,
  mappings,
  findCheckpoint,
  currentSensorBoot,
  lastSeq,
  lapTarget = null,
  debounceMs = DEFAULT_DEBOUNCE_MS,
  gpsAtStart = null,
  startedUtc = null,
  durable = true,
  now = Date.now(),
  statusMaxAgeMs = WIRELESS_STATUS_MAX_AGE_MS,
  cameraNode = null,
  ocrMinConf = DEFAULT_OCR_MIN_CONF,
}) {
  const nodes = {};
  let cursor = lastSeq;
  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    const checkpoint = findCheckpoint(node, clock.master_boot_id, clock.master_tick);
    const currentBoot = currentSensorBoot(node);
    if (!healthyCheckpoint(checkpoint) || (currentBoot != null && checkpoint.sensor_boot_id !== currentBoot) || now - checkpoint.received_at > statusMaxAgeMs) {
      throw new EngineError("checkpoint", `Sensor ${node}: Capture Confirmation Missing`, node);
    }
    nodes[node] = { boot: checkpoint.sensor_boot_id, seq: checkpoint.capture_seq, role: mapping.role, cpSeq: checkpoint.seq };
    cursor = Math.min(cursor, checkpoint.seq);
  }
  return {
    schema: RUN_SCHEMA,
    radioProto: RADIO_PROTO_VERSION,
    usbProto: USB_PROTO_VERSION,
    runId,
    mode,
    note,
    boundaryTick: String(clock.master_tick),
    masterBootId: clock.master_boot_id,
    nodes,
    cursor,
    ...(mode.startsWith("camera-") ? { cameraNode, decisions: {} } : {}),
    ...(mode === "camera-laps" ? { ocrMinConf, results: {} } : {}),
    lapTarget: MODE_KIND[mode] === "laps" && Number.isInteger(lapTarget) && lapTarget > 0 ? lapTarget : null,
    debounceMs: Number.isInteger(debounceMs) && debounceMs >= 0 ? debounceMs : DEFAULT_DEBOUNCE_MS,
    armed: true,
    closed: false,
    verification: "pending",
    classification: null,
    startTick: null,
    finishTick: null,
    crossingTicks: [],
    calibration: null,
    durationNs: null,
    fault: null,
    masterSessionEnded: null,
    gpsAtStart,
    startedUtc,
    startedAt: now,
    recordedAt: null,
    durable,
  };
}

// An explicit operator decision can exist without a device or a measured interval.
export function createClassifiedRun({ mode, note = "", classification, cursor = 0, now = Date.now(), durable = true }) {
  // A camera heat classifies its cars, never itself.
  if (!MODES.includes(mode) || mode === "camera-laps" || !RESULT_CLASSIFICATIONS.includes(classification)) {
    throw new EngineError("classification", "Invalid classification");
  }
  return {
    schema: RUN_SCHEMA, radioProto: RADIO_PROTO_VERSION, usbProto: USB_PROTO_VERSION,
    runId: uuid(), mode, note, classification,
    boundaryTick: null, masterBootId: null, nodes: {}, cursor,
    lapTarget: null, debounceMs: DEFAULT_DEBOUNCE_MS,
    armed: false, closed: true, verification: "pending",
    startTick: null, finishTick: null, crossingTicks: [],
    calibration: null, durationNs: null, fault: null, masterSessionEnded: null,
    gpsAtStart: null, startedUtc: null, startedAt: now, recordedAt: null, durable,
  };
}

function emptyCarResult() {
  return { crossingTicks: [], heldTicks: [], verification: "pending", classification: null, closed: false, recordedAt: null,
    calibration: null, durationNs: null, fault: null };
}

// A decided result: sprint S1 and F1; laps at least one lap once not pending.
function hasResult(run) {
  return MODE_KIND[run.mode] === "laps" ? run.verification !== "pending" && run.crossingTicks.length >= 2 : run.verification === "verified";
}

// History lists a run from its first decided result on (whatever later evaluations say) or while
// the operator's classification stands; a camera heat while one of its cars is such a record.
export function isRecord(run) {
  if (!run || run.deleted) return false;
  if (run.mode === "camera-laps") return Object.values(run.results || {}).some(isCarRecord);
  return run.recordedAt != null || RESULT_CLASSIFICATIONS.includes(run.classification);
}

export function isCarRecord(result) {
  return !!result && (result.recordedAt != null || RESULT_CLASSIFICATIONS.includes(result.classification));
}

export function runTouchedBy(run, rows) {
  return rows.some((row) => run.nodes[row.node_id] || row.node_id === "0" || row.kind === "quarantine");
}

function makeFault(run, reasons, kind, now) {
  return {
    fault_id: uuid(),
    mode: run.mode,
    run_id: run.runId,
    kind,
    occurred_at: new Date(now).toISOString(),
    reasons: Array.isArray(reasons) ? reasons : [],
  };
}

// STOP ends collection and verification, keeping the current result; DNS/DNF are never inferred.
export function stopRun(run) {
  return { ...run, armed: false, closed: true };
}

// Master timebase gone (reboot, version change): all sensors uncertain beyond what is identified.
export function endMasterSession(run, reason = MASTER_REBOOTED) {
  return run.masterSessionEnded ? run : { ...run, masterSessionEnded: { reason } };
}

export function shouldInvalidateOnMasterBoot(run, masterBootId) {
  return !!run && !run.closed && masterBootId != null && run.masterBootId !== masterBootId;
}

function tickText(v) {
  return v === Infinity ? "inf" : v == null ? null : String(v);
}

function reasonOf(cut) {
  const hole = cut.cutHole || {};
  const reason = hole.master ? hole.reason : `${cut.label || `Sensor ${cut.node_id}`}: ${hole.reason}`;
  return { node_id: hole.master ? "0" : cut.node_id, role: cut.role, reason, lo: tickText(hole.lo), hi: tickText(hole.hi) };
}

// What the evaluation shows of one accepted crossing.
function shown(node_id, role, tick, edges) {
  const crossing = { node_id, role, tick: String(tick) };
  const edge = edges.find((e) => e.tick === tick);
  if (!edge?.row.camera) return crossing;
  return { ...crossing, key: crossingKey(edge.row), autoTick: edge.row.master_tick, camera: edge.row.camera };
}

// Evaluate a run on every stored row since its cursor (pure) -> { run, crossings, laps }
export function evaluateRun(run, rows, { now = Date.now() } = {}) {
  const boundary = BigInt(run.boundaryTick);
  const debounceTicks = BigInt(run.debounceMs) * MASTER_TICKS_PER_MS;
  const timebaseEnd = masterTimebaseEnd(run, rows);
  const sessionEnd = run.masterSessionEnded?.reason || (timebaseEnd != null ? MASTER_TIMEBASE_ENDED : null);
  if (run.mode === "camera-laps") return evaluateHeat(run, rows, { now, boundary, debounceTicks, sessionEnd });
  const streams = Object.keys(run.nodes).map((node) => decideStream(sensorStream(run, node, rows, { sessionEnd }), run.decisions));
  // The role's sensor: its crossings above `after` and the first hole that may hide one.
  // The camera's crossings take no debounce: its detector already makes one crossing of one car.
  const role = (name, after) => {
    const stream = streams.find((s) => s.role === name);
    const camera = run.cameraNode === stream.node_id;
    return { node_id: stream.node_id, role: name, label: camera ? "Camera" : null, edges: stream.edges,
      ...acceptCrossings(stream, { after, debounceTicks: camera ? 0n : debounceTicks }) };
  };
  const crossings = [];
  const show = (r) => {
    for (const tick of r.accepted) crossings.push(shown(r.node_id, r.role, tick, r.edges));
  };

  let state = "pending";
  let faultCut = null;
  let startTick = null;
  let finishTick = null;
  let crossingTicks = [];
  const invalid = (r) => {
    state = "invalid";
    faultCut = r;
  };

  const start = role("start", boundary - 1n);
  show(start);
  if (MODE_KIND[run.mode] === "laps") {
    const ticks = start.accepted.map(String);
    const target = run.lapTarget;
    crossingTicks = ticks;
    if (target && ticks.length >= target + 1) {
      state = "verified";
      crossingTicks = ticks.slice(0, target + 1); // crossings past the target are no laps
    } else if (start.final) {
      invalid(start);
    } else if (ticks.length >= 2 && start.cut == null) {
      state = "verified"; // so far: the run stays open until its target or Stop
    }
  } else {
    const s1 = start.accepted[0] ?? null;
    if (s1 == null) {
      if (start.final) invalid(start);
    } else {
      // Finish edges at or before the first start are dropped before debouncing (no warning).
      const finish = role("finish", s1);
      show(finish);
      startTick = String(s1);
      const f1 = finish.accepted[0] ?? null;
      if (f1 != null) {
        state = "verified";
        finishTick = String(f1);
      } else if (finish.final) {
        invalid(finish);
        startTick = null;
      }
    }
  }

  // A verified sprint stays open: only Stop, lost timebase, invalid result or lap target end it.
  const decided = state === "invalid" || sessionEnd != null ||
    (state === "verified" && MODE_KIND[run.mode] === "laps" && run.lapTarget && crossingTicks.length === run.lapTarget + 1);
  let next = { ...run, verification: state, startTick, finishTick, crossingTicks };
  if (decided) next = { ...next, armed: false, closed: true };
  if (next.recordedAt == null && hasResult(next)) next.recordedAt = now;
  if (state === "invalid") next.fault = run.fault || makeFault(run, [reasonOf(faultCut)], "measurement", now);
  else if (state === "verified") next.fault = null;

  const laps = crossingTicks.slice(1).map((tick, i) => ({ startTick: crossingTicks[i], endTick: tick }));
  return { run: next, crossings, laps };
}

// camera-laps: every car's laps from the crossings assigned to it, against the camera's holes. A
// car is any number a crossing went to, or one the operator classified. More cars may still come,
// so only Stop or a lost timebase closes the heat.
// Camera crossings take no debounce (the detector already makes one crossing of one car): a number
// read again within the debounce of that car's last crossing is no lap but a misread or a split
// detection, so it stays unassigned for the operator.
// An unassigned crossing may be any car's, one not seen yet too. Every car whose laps it could
// change holds it (heldTicks): the lap around it may be two, its lap count one more, its best lap
// shorter. The laps' own times stand. Once the heat closes, a car still holding one is invalid.
function evaluateHeat(run, rows, { now, boundary, debounceTicks, sessionEnd }) {
  const node = Object.keys(run.nodes)[0];
  const stream = sensorStream(run, node, rows, { sessionEnd });
  const decisions = run.decisions || {};
  const byCar = new Map(Object.entries(run.results || {}).filter(([, r]) => RESULT_CLASSIFICATIONS.includes(r.classification)).map(([car]) => [car, []]));
  const lastTick = new Map();
  const crossings = [];
  const unassigned = [];
  for (const edge of stream.edges) {
    const decision = decisions[crossingKey(edge.row)];
    const tick = decidedTick(edge.row, decision);
    let car = decision?.ignore ? null : decidedCar(edge.row, decision, run);
    const operator = !!decision && "car" in decision;
    if (car != null && !operator && tick >= boundary && lastTick.has(car) && tick - lastTick.get(car) < debounceTicks) car = null;
    const state = decision?.ignore ? "ignored" : car != null ? "assigned" : "unassigned";
    crossings.push({ key: crossingKey(edge.row), node_id: node, role: "start", tick: String(tick), autoTick: edge.row.master_tick,
      camera: edge.row.camera, car: state === "assigned" ? car : null, state });
    if (state === "assigned") {
      if (!byCar.has(car)) byCar.set(car, []);
      byCar.get(car).push({ tick, row: edge.row });
      if (tick >= boundary) lastTick.set(car, tick);
    } else if (state === "unassigned" && tick >= boundary) unassigned.push(tick);
  }
  crossings.sort((a, b) => (BigInt(a.tick) < BigInt(b.tick) ? -1 : BigInt(a.tick) > BigInt(b.tick) ? 1 : 0));
  const closing = run.closed || sessionEnd != null;
  const results = {};
  const laps = {};
  let recordedAt = null;
  for (const car of [...byCar.keys()].sort((a, b) => Number(a) - Number(b))) {
    const prev = run.results?.[car] || emptyCarResult();
    const cut = { node_id: node, role: "start", label: "Camera",
      ...acceptCrossings({ ...stream, edges: byCar.get(car) }, { after: boundary - 1n, debounceTicks: 0n }) };
    let ticks = cut.accepted.map(String);
    const target = run.lapTarget;
    const reached = !!target && ticks.length >= target + 1;
    if (reached) ticks = ticks.slice(0, target + 1);
    // After a car's last lap, an unassigned crossing can no longer be one of its laps.
    const held = unassigned.filter((t) => !(reached && t > BigInt(ticks[target]))).map(String);
    let state = "pending";
    let faultCut = cut;
    if (reached && !held.length) state = "verified";
    else if (cut.final) state = "invalid";
    else if (held.length) {
      if (closing) {
        state = "invalid";
        faultCut = { ...cut, cutHole: { lo: BigInt(held[0]) - 1n, hi: BigInt(held[0]), reason: "Unassigned Crossing" } };
      }
    } else if (ticks.length >= 2 && cut.cut == null) state = "verified";
    const decided = state === "invalid" || RESULT_CLASSIFICATIONS.includes(prev.classification) || (state === "verified" && reached);
    const next = { ...prev, crossingTicks: ticks, heldTicks: held, verification: state, closed: !!(decided || closing) };
    if (next.recordedAt == null && state !== "pending" && ticks.length >= 2) next.recordedAt = now;
    if (state === "invalid") next.fault = prev.fault || makeFault(run, [reasonOf(faultCut)], "measurement", now);
    else if (state === "verified") next.fault = null;
    results[car] = next;
    laps[car] = ticks.slice(1).map((tick, i) => ({ startTick: ticks[i], endTick: tick }));
    if (next.recordedAt != null && (recordedAt == null || next.recordedAt < recordedAt)) recordedAt = next.recordedAt;
  }
  let next = { ...run, results, recordedAt };
  if (closing) next = { ...next, armed: false, closed: true };
  return { run: next, crossings, laps };
}
