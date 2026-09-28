/* Run engine — pure port of the FSK server timing service (timing.mjs), reduced to two mechanisms.
 *
 * A run is a plain object (JSON-able via run-codec):
 * {
 *   version, runId, mode: "sprint" | "laps", note,
 *   boundaryTick: string, masterBootId: number,
 *   nodes: { [node_id]: { boot, seq, role } },   // capture frontier at START
 *   cursor: number,                              // eventLog seq; evidence = rows with seq > cursor
 *   lapTarget: number|null,                      // laps: auto-stop after this many laps
 *   calib: null | { ppb, ppsTick, utc, fix, sats, span },  // GPS PPS calibration frozen at START
 *   armed: boolean,                              // light green
 *   closed: boolean,                             // no more evidence accepted
 *   verification: "pending" | "verified" | "invalid",
 *   lapTicks: bigint[], result: number|null,
 *   fault: null | { fault_id, mode, run_id, kind, occurred_at, reasons: [{ node_id, role?, reason }] },
 *   startedAt: number, historyId: number|null,
 * }
 */
import { verifyCaptures, CAPTURE_HEALTH, WIRELESS_PROTOCOL_VERSION } from "./capture-integrity";
import { masterTickDelta, masterTickDeltaMs, masterTickDurationsMs, masterTickDistanceBelowMs } from "./event-timing";
import { WIRELESS_STATUS_MAX_AGE_MS } from "./constants";

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

// START. Every mapped node needs a healthy checkpoint (flags 15 set, same sensor boot,
// received within statusMaxAgeMs) at or before the boundary tick; the run's evidence
// window opens just before the oldest such checkpoint.
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
  calibration = null,
  now = Date.now(),
  statusMaxAgeMs = WIRELESS_STATUS_MAX_AGE_MS,
}) {
  const nodes = {};
  let cursor = lastSeq;
  for (const mapping of mappings) {
    const node = String(mapping.node_id);
    const checkpoint = findCheckpoint(node, clock.master_boot_id, clock.master_tick);
    const currentBoot = currentSensorBoot(node);
    if (
      !checkpoint ||
      (currentBoot != null && checkpoint.sensor_boot_id !== currentBoot) ||
      (checkpoint.flags & CAPTURE_HEALTH) !== CAPTURE_HEALTH ||
      now - checkpoint.received_at > statusMaxAgeMs
    ) {
      throw new EngineError("checkpoint", `Sensor ${node} has not confirmed its latest captures yet. Wait a few seconds and start again.`, node);
    }
    nodes[node] = { boot: checkpoint.sensor_boot_id, seq: checkpoint.capture_seq, role: mapping.role };
    cursor = Math.min(cursor, checkpoint.seq);
  }
  return {
    version: WIRELESS_PROTOCOL_VERSION,
    runId,
    mode,
    note,
    boundaryTick: String(clock.master_tick),
    masterBootId: clock.master_boot_id,
    nodes,
    cursor,
    lapTarget: mode === "laps" && Number.isInteger(lapTarget) && lapTarget > 0 ? lapTarget : null,
    calib: calibration && Number.isInteger(calibration.ppb) ? { ...calibration } : null,
    armed: true,
    closed: false,
    verification: "pending",
    lapTicks: [],
    result: null,
    fault: null,
    startedAt: now,
    historyId: null,
  };
}

export function runTouchedBy(run, rows) {
  return rows.some((row) => run.nodes[row.node_id] || row.node_id === "0");
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

// Disarming blocks a new interval; closing also prevents late proof recovery.
export function invalidateRun(run, reasons, { awaitEvidence = false, kind = "quality", now = Date.now() } = {}) {
  return {
    ...run,
    armed: false,
    closed: !awaitEvidence,
    verification: "invalid",
    fault: run.fault || makeFault(run, reasons, kind, now),
  };
}

export function closeRun(run) {
  return { ...run, armed: false, closed: true };
}

// Master reboot while armed ends the run (evidence may still confirm an earlier interval).
export function shouldInvalidateOnMasterBoot(run, masterBootId) {
  return !!run && run.armed && !run.closed && masterBootId != null && run.masterBootId !== masterBootId;
}

// Evaluate one run against every stored row since its cursor. Pure: returns the next run
// plus the accepted (debounced) crossings for display.
export function evaluateRun(run, rows, debounceMs, now = Date.now()) {
  const verified = verifyCaptures(run, rows);
  const debounce = {};
  const accepted = verified.events.filter((ev) => {
    const last = debounce[ev.node_id];
    if (last != null && masterTickDistanceBelowMs(ev.master_tick, last, debounceMs)) return false;
    debounce[ev.node_id] = ev.master_tick;
    return true;
  });
  let result = null;
  let complete = false;
  let invalidDuration = false;
  const laps = [];
  const ppb = run.calib?.ppb ?? 0;
  if (run.mode === "laps") {
    const crossings = accepted.filter((ev) => ev.role === "start");
    for (let i = 1; i < crossings.length; i++) {
      if (run.lapTarget && laps.length >= run.lapTarget) break;
      const duration = masterTickDelta(crossings[i].master_tick, crossings[i - 1].master_tick);
      if (duration <= 0n) {
        invalidDuration = true;
        break;
      }
      laps.push(duration);
    }
    if (laps.length) result = masterTickDurationsMs(laps, ppb);
    complete = !!run.lapTarget && laps.length >= run.lapTarget;
  } else {
    const start = accepted.find((ev) => ev.role === "start");
    const finish = accepted.find((ev) => ev.role === "finish");
    if (start && finish) {
      const duration = masterTickDelta(finish.master_tick, start.master_tick);
      invalidDuration = duration <= 0n;
      if (!invalidDuration) {
        result = masterTickDeltaMs(finish.master_tick, start.master_tick, ppb);
        complete = true;
      }
    }
  }
  let next = {
    ...run,
    lapTicks: laps,
    result,
    verification: !complete && run.fault ? "invalid" : result == null ? "pending" : "verified",
  };
  if (complete) {
    next.armed = false;
    next.closed = true;
    next.fault = null;
  }
  // A completed, verified interval before a later fault stays official.
  if (!complete && (verified.fault || invalidDuration)) {
    next = invalidateRun(
      next,
      [verified.fault || { node_id: null, reason: "The raw start→finish tick difference is not positive." }],
      { awaitEvidence: !!verified.fault && !invalidDuration, kind: "measurement", now },
    );
  }
  return { run: next, accepted, laps, result, complete, invalidDuration, fault: verified.fault, throughTick: verified.throughTick };
}
