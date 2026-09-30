/* The single run: START sequence, evidence evaluation, calibration freeze, live elapsed clock. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { useDeviceStore } from "./device";
import { useSettingsStore } from "./settings";
import { useHistoryStore } from "./history";
import * as eventLog from "../lib/eventLog";
import { createRun, evaluateRun, stopRun, endMasterSession, shouldInvalidateOnMasterBoot, runTouchedBy, EngineError, MASTER_REBOOTED } from "../lib/engine";
import { wirelessQuality, missingRoles as missingRolesOf } from "../lib/quality";
import { encodeRun, decodeRun } from "../lib/run-codec";
import { MODES, CALIBRATION_WAIT_MS } from "../lib/constants";
import { buildTimeline, freezeCalibration, utcMsAt, durationNs } from "../lib/calibration";
import { resultTicks, resultNs as resultNsOf, lapsNs as lapsNsOf, spanNs, calibrationMethod as calibrationMethodOf } from "../lib/results";
import { formatDuration } from "../lib/format";

const KEY_RUN = "tk.run.v3";
const KEY_RUN_OLD = "tk.run.v2";
const CP_RETRY_MS = 2000; // re-ask for checkpoints while a stopped run waits (a sensor may miss the beacons)
export const VERSION_CHANGED = "The master reports another protocol version.";

function makeLive() {
  return {
    clockDisplay: "00:00.000",
    raf: null,
    startedAt: null, // wall ms of the first start crossing (elapsed-clock display only)
    crossings: [], // engine crossings: { node_id, role, tick, confirmed }
    laps: [], // engine laps: { startTick, endTick, confirmed }
  };
}

export const useTimingStore = defineStore("timing", () => {
  const notyf = useNotification();
  const run = ref(null);
  const live = reactive(makeLive());
  const starting = ref(false);
  let cpTimer = null; // checkpoint requests for a stopped run awaiting confirmation
  let freezeTimer = null;
  let freezeWait = null; // { runId, lastTick } while a decided result waits for its next PPS edge
  let pendingHistorySync = false; // a restored run closed by a protocol change

  const armed = computed(() => !!run.value?.armed);
  const fault = computed(() => run.value?.fault ?? null);
  const lightColor = computed(() => (!run.value ? "grey" : run.value.armed ? "green" : "red"));

  // PPS timeline of the run's master boot (provisional calibration until frozen).
  const timeline = computed(() => {
    const device = useDeviceStore();
    const r = run.value;
    return buildTimeline(r && device.ppsBootId === r.masterBootId ? device.ppsEdges : []);
  });

  const resultNs = computed(() => (run.value ? resultNsOf(run.value, timeline.value) : null));
  // Display laps (engine order); ns from the frozen calibration or the provisional timeline.
  const lapRows = computed(() => {
    const r = run.value;
    if (!r) return [];
    if (r.closed && !live.laps.length) return lapsNsOf(r, timeline.value).map((ns) => ({ ns, confirmed: true }));
    return live.laps.map((lap) => ({ ns: spanNs(r, lap.startTick, lap.endTick, timeline.value), confirmed: lap.confirmed }));
  });
  const calibrationMethod = computed(() => (run.value ? calibrationMethodOf(run.value, timeline.value) : null));

  // Crossing list of a role with the time since the first start crossing.
  function crossingRows(role) {
    const r = run.value;
    const list = live.crossings.filter((c) => c.role === role);
    const first = live.crossings.find((c) => c.role === "start");
    if (!r || !first) return list.map((c) => ({ ...c, key: `${c.node_id}:${c.tick}`, ns: null }));
    return list.map((c) => ({ ...c, key: `${c.node_id}:${c.tick}`, ns: spanNs(r, first.tick, c.tick, timeline.value) }));
  }

  function persist() {
    try {
      if (run.value) localStorage.setItem(KEY_RUN, encodeRun(run.value));
      else localStorage.removeItem(KEY_RUN);
    } catch {
      /* ignore */
    }
  }

  function openCursor() {
    return run.value && !run.value.closed ? run.value.cursor : Infinity;
  }

  // Restore the stored run before the event log loads so its cursor is protected.
  function restore() {
    run.value = null;
    try {
      let decoded = decodeRun(localStorage.getItem(KEY_RUN));
      if (!decoded) {
        decoded = decodeRun(localStorage.getItem(KEY_RUN_OLD));
        if (decoded) localStorage.removeItem(KEY_RUN_OLD);
      }
      if (decoded && MODES.includes(decoded.run.mode)) {
        run.value = decoded.run;
        pendingHistorySync = decoded.protocolChanged;
        persist();
      }
    } catch {
      run.value = null;
    }
    return openCursor();
  }

  function setRun(next) {
    run.value = next;
    persist();
    eventLog.protect(openCursor());
  }

  function stopClock() {
    if (live.raf) cancelAnimationFrame(live.raf);
    live.raf = null;
  }
  function startClock() {
    stopClock();
    const tick = () => {
      if (live.startedAt != null) live.clockDisplay = formatDuration(BigInt(Math.max(0, Math.floor(Date.now() - live.startedAt))) * 1_000_000n);
      live.raf = requestAnimationFrame(tick);
    };
    live.raf = requestAnimationFrame(tick);
  }
  function showResultOnClock() {
    const ns = resultNs.value;
    if (ns != null) live.clockDisplay = formatDuration(ns);
  }
  function resetLive() {
    stopClock();
    Object.assign(live, makeLive());
  }

  function stopCheckpointRequests() {
    if (cpTimer) clearInterval(cpTimer);
    cpTimer = null;
  }
  // A stopped run closes when the needed roles are certain through the stop tick; ask the
  // master for checkpoints now and every CP_RETRY_MS until it does (Stop makes no radio event).
  function requestCheckpointsUntilClosed(runId) {
    stopCheckpointRequests();
    const ask = () => {
      const current = run.value;
      if (!current || current.runId !== runId || current.closed || current.stopTick == null) return stopCheckpointRequests();
      useDeviceStore().requestCheckpoint();
    };
    ask();
    cpTimer = setInterval(ask, CP_RETRY_MS);
  }

  function qualityFor(mode, now = Date.now()) {
    const device = useDeviceStore();
    const settings = useSettingsStore();
    return wirelessQuality({
      mode,
      mappings: settings.mappingsFor(mode),
      telemetry: device.telemetry,
      masterFresh: device.masterFresh(now),
      contract: device.contract,
      pipeline: device.pipelineHealth(now),
      now,
    });
  }

  function missingRoles(mode) {
    return missingRolesOf(mode, useSettingsStore().mappingsFor(mode));
  }

  // START: quality -> fresh master clock -> quality again -> run from checkpoints -> catch up.
  async function start(mode, note = "") {
    if (starting.value) return false;
    const device = useDeviceStore();
    const settings = useSettingsStore();
    const history = useHistoryStore();
    if (!MODES.includes(mode)) {
      notyf.error("Invalid mode.");
      return false;
    }
    if (run.value?.armed) {
      notyf.error("A measurement is already running.");
      return false;
    }
    starting.value = true;
    try {
      let q = qualityFor(mode);
      if (!q.ok) {
        notyf.error(q.reasons[0].reason);
        return false;
      }
      const cursorBefore = eventLog.getLastSeq();
      const clock = await device.readClock();
      q = qualityFor(mode);
      if (!q.ok) {
        notyf.error(q.reasons[0].reason);
        return false;
      }
      const edges = device.ppsBootId === clock.master_boot_id ? device.ppsEdges : [];
      const utcMs = utcMsAt(edges, clock.master_tick);
      const next = createRun({
        mode,
        note,
        clock,
        mappings: q.mappings,
        findCheckpoint: (node, boot, maxTick) => eventLog.latestCheckpoint(node, boot, maxTick),
        currentSensorBoot: (node) => device.telemetry[node]?.sensor_boot_id ?? null,
        lastSeq: cursorBefore,
        lapTarget: mode === "laps" ? settings.state.lapTarget : null,
        debounceMs: settings.state.debounceMs,
        gpsAtStart: device.gpsReport(),
        startedUtc: utcMs != null ? new Date(utcMs).toISOString() : null,
      });
      stopCheckpointRequests();
      cancelFreeze();
      resetLive();
      settings.rememberNote(note);
      setRun(next);
      next.historyId = await history.open(next, { masterDevId: device.identity?.devid || null });
      if (run.value?.runId === next.runId) setRun({ ...run.value, historyId: next.historyId });
      // Edges captured during the clock read belong to this run.
      onEventRows(eventLog.since(next.cursor));
      return true;
    } catch (e) {
      notyf.error(e instanceof EngineError ? e.message : `Start failed: ${e.message || e}`);
      return false;
    } finally {
      starting.value = false;
    }
  }

  // STOP: fence at the master's current tick. Only the master's own answer (`T`) can be the
  // fence: it is captured after the press, so nothing that crossed before Stop falls outside
  // it. A PC-side estimate lags the master by the delivery delay, so without `T` Stop is refused.
  async function stop() {
    const current = run.value;
    if (!current?.armed) return false;
    const device = useDeviceStore();
    if (!device.connected) {
      notyf.error("The master is not connected. Reconnect to stop, or Clear to discard the run.");
      return false;
    }
    let stopTick = null;
    try {
      const clock = await device.readClock();
      if (clock.master_boot_id === current.masterBootId) stopTick = BigInt(clock.master_tick);
    } catch {
      /* refused below */
    }
    const latest = run.value;
    if (!latest || latest.runId !== current.runId || !latest.armed) return false;
    if (stopTick == null) {
      notyf.error("The master did not answer the clock request. Try Stop again, or Clear to discard the run.");
      return false;
    }
    applyEvaluation(evaluateRun(stopRun(latest, stopTick), eventLog.since(latest.cursor)));
    if (run.value && !run.value.closed) requestCheckpointsUntilClosed(latest.runId);
    return true;
  }

  function reset() {
    stopCheckpointRequests();
    cancelFreeze();
    resetLive();
    setRun(null);
  }

  function cancelFreeze() {
    if (freezeTimer) clearTimeout(freezeTimer);
    freezeTimer = null;
    freezeWait = null;
  }

  // Freeze the calibration of a decided run's ticks (the result and any confirmed laps).
  function freezeNow(runId) {
    cancelFreeze();
    const current = run.value;
    if (!current || current.runId !== runId || current.calibration) return;
    const ticks = resultTicks(current);
    if (!ticks.length) return;
    const calibration = freezeCalibration(timeline.value, ticks);
    let ns = null;
    if (current.verification === "verified") {
      if (current.mode === "sprint") ns = durationNs(calibration.points, current.startTick, current.finishTick);
      else if (current.totalValid && ticks.length >= 2) ns = durationNs(calibration.points, ticks[0], ticks[ticks.length - 1]);
    }
    setRun({ ...current, calibration, durationNs: ns == null ? null : String(ns) });
    showResultOnClock();
    useHistoryStore().upsert(run.value, { throughSeq: eventLog.getLastSeq() });
  }

  // A decided result waits for a qualified PPS edge after its last tick, at most CALIBRATION_WAIT_MS.
  function scheduleFreeze() {
    const current = run.value;
    if (!current?.closed || current.calibration) return;
    const ticks = resultTicks(current);
    if (!ticks.length) return;
    const lastTick = ticks.reduce((m, t) => (BigInt(t) > m ? BigInt(t) : m), 0n);
    const device = useDeviceStore();
    const edges = device.ppsBootId === current.masterBootId ? device.ppsEdges : [];
    if (edges.some((e) => BigInt(e.tick) > lastTick)) return freezeNow(current.runId);
    cancelFreeze();
    freezeWait = { runId: current.runId, lastTick, masterBootId: current.masterBootId };
    freezeTimer = setTimeout(() => freezeNow(current.runId), CALIBRATION_WAIT_MS);
  }

  function onPpsEdge(edge) {
    if (!freezeWait || edge.master_boot_id !== freezeWait.masterBootId || BigInt(edge.tick) <= freezeWait.lastTick) return;
    freezeNow(freezeWait.runId);
  }

  function applyEvaluation(result) {
    const next = result.run;
    const before = run.value;
    live.crossings = result.crossings;
    live.laps = result.laps;
    const firstStart = result.crossings.find((c) => c.role === "start");
    if (firstStart && live.startedAt == null && next.armed) {
      live.startedAt = useDeviceStore().tickToWallMs(firstStart.tick);
      startClock();
    }
    setRun(next);
    if (next.closed || !next.armed) {
      stopClock();
      showResultOnClock();
    }
    if (next.closed) stopCheckpointRequests();
    if (next.closed && !(before?.closed && before.runId === next.runId)) scheduleFreeze();
    const changed =
      !before ||
      before.runId !== next.runId ||
      before.verification !== next.verification ||
      before.closed !== next.closed ||
      before.armed !== next.armed ||
      before.startTick !== next.startTick ||
      before.finishTick !== next.finishTick ||
      (before.crossingTicks || []).length !== next.crossingTicks.length;
    if (changed) useHistoryStore().upsert(next, { throughSeq: eventLog.getLastSeq() });
    if (next.verification === "invalid" && next.fault && (!before || !before.fault)) notyf.error(next.fault.reasons?.[0]?.reason || "Measurement fault");
  }

  function evaluateOpen(current = run.value) {
    if (!current || current.closed) return;
    applyEvaluation(evaluateRun(current, eventLog.since(current.cursor)));
  }

  // New rows arrived (or the run was just opened): re-evaluate if they touch the open run.
  function onEventRows(rows) {
    const current = run.value;
    if (!rows.length || !current || current.closed) return;
    if (!runTouchedBy(current, rows)) return;
    evaluateOpen(current);
  }

  // The master's timebase is gone: an open run keeps what is already certain and nothing more.
  function onMasterBoot(masterBootId) {
    const current = run.value;
    if (!shouldInvalidateOnMasterBoot(current, masterBootId)) return;
    evaluateOpen(endMasterSession(current, MASTER_REBOOTED));
    if (run.value?.verification === "invalid") notyf.error(MASTER_REBOOTED);
  }

  function onMasterVersion({ usbProto, radioProto }) {
    const current = run.value;
    if (!current || current.closed) return;
    if (usbProto === current.usbProto && radioProto === current.radioProto) return;
    evaluateOpen(endMasterSession(current, VERSION_CHANGED));
  }

  // Storage failed: the run and its history row are no longer durable.
  function markNonDurable() {
    const current = run.value;
    if (!current || current.durable === false) return;
    setRun({ ...current, durable: false });
    useHistoryStore().upsert(run.value, {});
  }

  // After the event log is loaded: rebuild the live view of a restored run.
  async function reevaluate() {
    const current = run.value;
    resetLive();
    if (!current) return;
    const device = useDeviceStore();
    if (current.masterBootId != null && !device.connected) await device.useEdgesOf(current.masterBootId);
    if (pendingHistorySync) {
      pendingHistorySync = false;
      await useHistoryStore().upsert(current, {});
    }
    if (current.closed) {
      if (current.mode === "sprint") {
        live.crossings = [
          ...(current.startTick ? [{ node_id: "", role: "start", tick: current.startTick, confirmed: true }] : []),
          ...(current.finishTick ? [{ node_id: "", role: "finish", tick: current.finishTick, confirmed: true }] : []),
        ];
      } else {
        const c = current.crossingTicks || [];
        live.crossings = c.map((tick) => ({ node_id: "", role: "start", tick, confirmed: true }));
        live.laps = c.slice(1).map((tick, i) => ({ startTick: c[i], endTick: tick, confirmed: true }));
      }
      showResultOnClock();
      scheduleFreeze();
      return;
    }
    // A restored run cannot show a live wall clock (the wall/tick anchor is gone).
    evaluateOpen(current);
    stopClock();
    live.startedAt = null;
    showResultOnClock();
    const restored = run.value;
    if (restored && !restored.closed && restored.stopTick != null) requestCheckpointsUntilClosed(restored.runId);
  }

  return {
    run,
    live,
    starting,
    armed,
    fault,
    lightColor,
    timeline,
    resultNs,
    lapRows,
    calibrationMethod,
    crossingRows,
    restore,
    reevaluate,
    start,
    stop,
    reset,
    onEventRows,
    onMasterBoot,
    onMasterVersion,
    onPpsEdge,
    markNonDurable,
    qualityFor,
    missingRoles,
  };
});
