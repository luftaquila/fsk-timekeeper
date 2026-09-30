/* Run engine: START snapshot, evidence evaluation and run states (PLAN §2.4).
 *
 * A run is a plain JSON-able object:
 * {
 *   schema, radioProto, usbProto, runId, mode: "sprint" | "laps", note,
 *   boundaryTick: string, masterBootId: number,
 *   nodes: { [node_id]: { boot, seq, role, cpSeq } },   // START checkpoint per mapped sensor
 *   cursor: number,                                    // evidence = event-log rows with seq > cursor
 *   lapTarget: number|null, debounceMs: number,        // frozen at START
 *   stopTick: string|null,                             // Stop fence (master `T` answer)
 *   armed, closed,
 *   verification: "pending" | "verified" | "invalid" | "dnf", dnfReason: "DNF" | "DNS" | null,
 *   startTick, finishTick: string|null,                // sprint result S1, F1
 *   crossingTicks: string[], totalValid: boolean,       // laps: confirmed crossings C0..Ck
 *   calibration: null | frozen descriptor (calibration.js), durationNs: string|null,
 *   fault: null | { fault_id, mode, run_id, kind, occurred_at, reasons: [{ node_id, role, reason, lo, hi }] },
 *   masterSessionEnded: null | { reason },             // master reboot / version change seen while open
 *   gpsAtStart, startedUtc, startedAt, historyId, durable,
 * }
 * A result depends only on its own interval: sprint on start through S1 and finish through
 * F1, laps on the start role through the last lap. Holes elsewhere do not matter; a possible
 * missed crossing inside the interval keeps the result unconfirmed (fail-closed).
 */
import { sensorStream, masterTimebaseEnd } from "./sensor-stream";
import { roleStream } from "./role-stream";
import { FLAG_HEALTHY, FLAG_TIME_UNKNOWN, RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "./protocol";
import { MASTER_TICKS_PER_MS } from "./event-timing";
import { WIRELESS_STATUS_MAX_AGE_MS, DEFAULT_DEBOUNCE_MS } from "./constants";

export const RUN_SCHEMA = 3;
export const MASTER_TIMEBASE_ENDED = "The master timebase ended during the run.";
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
}) {
  const nodes = {};
  let cursor = lastSeq;
  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    const checkpoint = findCheckpoint(node, clock.master_boot_id, clock.master_tick);
    const currentBoot = currentSensorBoot(node);
    if (!healthyCheckpoint(checkpoint) || (currentBoot != null && checkpoint.sensor_boot_id !== currentBoot) || now - checkpoint.received_at > statusMaxAgeMs) {
      throw new EngineError("checkpoint", `Sensor ${node} has not confirmed its latest captures yet. Wait a few seconds and start again.`, node);
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
    lapTarget: mode === "laps" && Number.isInteger(lapTarget) && lapTarget > 0 ? lapTarget : null,
    debounceMs: Number.isInteger(debounceMs) && debounceMs >= 0 ? debounceMs : DEFAULT_DEBOUNCE_MS,
    stopTick: null,
    armed: true,
    closed: false,
    verification: "pending",
    dnfReason: null,
    startTick: null,
    finishTick: null,
    crossingTicks: [],
    totalValid: true,
    calibration: null,
    durationNs: null,
    fault: null,
    masterSessionEnded: null,
    gpsAtStart,
    startedUtc,
    startedAt: now,
    historyId: null,
    durable,
  };
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

// Close as invalid with explicit reasons (protocol change, lost storage, ...).
export function invalidateRun(run, reasons, { kind = "quality", now = Date.now() } = {}) {
  return {
    ...run,
    armed: false,
    closed: true,
    verification: "invalid",
    dnfReason: null,
    totalValid: run.mode === "laps" ? false : run.totalValid,
    fault: run.fault || makeFault(run, reasons, kind, now),
  };
}

// STOP. Disarms at the master's stop tick: crossings up to it still count when they arrive
// late, later ones are ignored; the run closes once the needed roles are certain through it.
export function stopRun(run, stopTick) {
  return { ...run, armed: false, stopTick: String(stopTick) };
}

// The master's timebase is gone (reboot, version change): every sensor is uncertain beyond
// what is already identified.
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
  const hole = cut.hole || {};
  const reason = hole.master ? hole.reason : `Sensor ${cut.node_id} ${hole.reason}.`;
  return { node_id: hole.master ? "0" : cut.node_id, role: cut.role, reason, lo: tickText(hole.lo), hi: tickText(hole.hi) };
}

/* Evaluate a run against every stored row since its cursor. Pure.
 * -> { run, crossings: [{node_id, role, tick, confirmed}], laps: [{startTick, endTick, confirmed}],
 *      certain: {start, finish} }
 */
export function evaluateRun(run, rows, { now = Date.now() } = {}) {
  const boundary = BigInt(run.boundaryTick);
  const debounceTicks = BigInt(run.debounceMs) * MASTER_TICKS_PER_MS;
  const stop = run.stopTick != null ? BigInt(run.stopTick) : null;
  const timebaseEnd = masterTimebaseEnd(run, rows);
  const sessionEnd = run.masterSessionEnded?.reason || (timebaseEnd != null ? MASTER_TIMEBASE_ENDED : null);
  const streams = Object.keys(run.nodes).map((node) => sensorStream(run, node, rows, { sessionEnd }));
  const members = (role) => streams.filter((stream) => stream.role === role);
  const within = (list) => (stop == null ? list : list.filter((c) => c.tick <= stop));

  const startRole = roleStream(members("start"), { after: boundary - 1n, debounceTicks });
  const starts = within(startRole.accepted);
  const display = [];
  const push = (list, role, certain) => {
    for (const c of list) display.push({ node_id: c.node_id, role, tick: String(c.tick), confirmed: c.tick <= certain });
  };
  push(starts, "start", startRole.certain);

  let state = "pending";
  let dnfReason = null;
  let faultCuts = null;
  let startTick = null;
  let finishTick = null;
  let crossingTicks = [];
  let totalValid = true;
  let finishCertain = null;
  const invalid = (cuts) => {
    state = "invalid";
    faultCuts = cuts;
  };
  const cutsBelow = (role, bound) => role.finalCuts.filter((c) => bound == null || c.cut < bound);
  // A hole decides only once its role has settled: a sensor still behind may yet report the
  // crossing the result needs, before the hole.
  const cutBelow = (role, bound) => role.settled && (bound == null || role.finalCut < bound);

  if (run.mode === "laps") {
    const confirmed = starts.filter((c) => c.tick <= startRole.certain).map((c) => String(c.tick));
    const target = run.lapTarget;
    crossingTicks = confirmed;
    if (target && confirmed.length >= target + 1) {
      state = "verified";
      crossingTicks = confirmed.slice(0, target + 1);
    } else if (cutBelow(startRole, stop)) {
      invalid(cutsBelow(startRole, stop));
      totalValid = false;
    } else if (stop != null && startRole.certain >= stop) {
      if (confirmed.length >= 2) state = "verified";
      else {
        state = "dnf";
        dnfReason = confirmed.length === 1 ? "DNF" : "DNS";
      }
    } else if (stop == null && confirmed.length >= 2 && !startRole.cuts.length) {
      state = "verified"; // so far: the run stays open until its target or Stop
    }
  } else {
    const s1 = starts[0]?.tick ?? null;
    let s1Final = false;
    if (s1 != null) {
      if (s1 <= startRole.certain) s1Final = true;
      else if (cutBelow(startRole, s1)) invalid(cutsBelow(startRole, s1));
    } else if (cutBelow(startRole, stop)) {
      invalid(cutsBelow(startRole, stop));
    } else if (stop != null && startRole.certain >= stop) {
      state = "dnf";
      dnfReason = "DNS";
    }
    // Finish edges at or before the first start are dropped before debouncing (no warning).
    if (s1 != null) {
      const finishRole = roleStream(members("finish"), { after: s1, debounceTicks });
      const finishes = within(finishRole.accepted);
      push(finishes, "finish", finishRole.certain);
      finishCertain = finishRole.certain;
      if (s1Final) {
        startTick = String(s1);
        const f1 = finishes[0]?.tick ?? null;
        if (f1 != null) {
          if (f1 <= finishRole.certain) {
            state = "verified";
            finishTick = String(f1);
          } else if (cutBelow(finishRole, f1)) invalid(cutsBelow(finishRole, f1));
        } else if (cutBelow(finishRole, stop)) {
          invalid(cutsBelow(finishRole, stop));
        } else if (stop != null && finishRole.certain >= stop) {
          state = "dnf";
          dnfReason = "DNF";
        }
        if (state !== "verified") finishTick = null;
      }
    }
    if (state !== "verified") startTick = state === "dnf" && dnfReason === "DNF" ? String(s1) : null;
  }

  const decided = state === "invalid" || state === "dnf" || (state === "verified" && (run.mode === "sprint" || (run.lapTarget && crossingTicks.length === run.lapTarget + 1) || stop != null));
  let next = { ...run, verification: state, dnfReason, startTick, finishTick, crossingTicks, totalValid };
  if (decided) next = { ...next, armed: false, closed: true };
  if (state === "invalid") next.fault = run.fault || makeFault(run, (faultCuts || []).map(reasonOf), "measurement", now);
  else if (decided) next.fault = null;

  const laps = [];
  if (run.mode === "laps") {
    const end = run.lapTarget ? Math.min(starts.length, run.lapTarget + 1) : starts.length; // crossings past the target are no laps
    for (let i = 1; i < end; i++) {
      laps.push({ startTick: String(starts[i - 1].tick), endTick: String(starts[i].tick), confirmed: starts[i].tick <= startRole.certain });
    }
  }
  return { run: next, crossings: display, laps, certain: { start: startRole.certain, finish: finishCertain } };
}
