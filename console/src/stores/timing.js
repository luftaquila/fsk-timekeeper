/* The single run: START sequence, evidence evaluation, calibration freeze, live elapsed clock. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { useDeviceStore } from "./device";
import { useSettingsStore } from "./settings";
import { useHistoryStore } from "./history";
import * as eventLog from "../lib/eventLog";
import * as ppsLog from "../lib/ppsLog";
import { createRun, createClassifiedRun, evaluateRun, stopRun, endMasterSession, shouldInvalidateOnMasterBoot, runTouchedBy, EngineError, MASTER_REBOOTED } from "../lib/engine";
import { wirelessQuality, missingRoles as missingRolesOf } from "../lib/quality";
import { encodeRun, decodeRun } from "../lib/run-codec";
import * as runLog from "../lib/runLog";
import { MODES, CALIBRATION_WAIT_MS, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { buildTimeline, freezeCalibration, utcMsAt, durationNs } from "../lib/calibration";
import { resultTicks, resultNs as resultNsOf, lapsNs as lapsNsOf, spanNs } from "../lib/results";
import { formatDuration } from "../lib/format";

const VERSION_CHANGED = "The master reports another protocol version.";

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
  const classifying = ref(false);
  const freezes = new Map(); // runId -> timer; independent of the selected run
  let generation = runLog.epoch();
  let restored = false;

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

  // Crossing list of a role with the time since the first start crossing.
  function crossingRows(role) {
    const r = run.value;
    const list = live.crossings.filter((c) => c.role === role);
    const first = live.crossings.find((c) => c.role === "start");
    if (!r || !first) return list.map((c) => ({ ...c, key: `${c.node_id}:${c.tick}`, ns: null }));
    return list.map((c) => ({ ...c, key: `${c.node_id}:${c.tick}`, ns: spanNs(r, first.tick, c.tick, timeline.value) }));
  }

  function persist() {
    return runLog.save(run.value, { select: true });
  }

  function openCursor() {
    return run.value && !run.value.closed ? run.value.cursor : Infinity;
  }

  function setRun(next) {
    run.value = next;
    const saved = persist();
    eventLog.protect(openCursor());
    return saved;
  }

  async function syncHistory(current, options = {}) {
    const id = await useHistoryStore().upsert(current, options);
    if (id != null && run.value?.runId === current.runId && run.value.historyId !== id) {
      setRun({ ...run.value, historyId: id });
    }
  }

  function stopClock() {
    if (live.raf) cancelAnimationFrame(live.raf);
    live.raf = null;
  }
  function updateClock() {
    if (live.startedAt != null) live.clockDisplay = formatDuration(BigInt(Math.max(0, Math.floor(Date.now() - live.startedAt))) * 1_000_000n);
  }
  function startClock() {
    stopClock();
    const tick = () => {
      updateClock();
      live.raf = requestAnimationFrame(tick);
    };
    live.raf = requestAnimationFrame(tick);
  }
  function showStoppedClock() {
    if (run.value?.stoppedClockDisplay != null) {
      live.clockDisplay = run.value.stoppedClockDisplay;
      return;
    }
    const ns = resultNs.value;
    if (ns != null) live.clockDisplay = formatDuration(ns);
  }
  function resetLive() {
    stopClock();
    Object.assign(live, makeLive());
  }

  function qualityFor(mode, now = Date.now()) {
    const device = useDeviceStore();
    const settings = useSettingsStore();
    return wirelessQuality({
      mode,
      mappings: settings.mappingsFor(mode),
      telemetry: device.telemetry,
      masterFresh: device.masterFresh(now),
      connected: device.connected,
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
    if (starting.value || classifying.value || useHistoryStore().importing || useHistoryStore().clearing) return false;
    const device = useDeviceStore();
    if (device.updateInProgress) return false;
    const settings = useSettingsStore();
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
        durable: device.durable,
      });
      resetLive();
      settings.rememberNote(note);
      next.masterDevId = device.identity?.devid || null;
      await setRun(next);
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

  // Like the FSK service, Stop closes the current snapshot without assigning DNS/DNF.
  async function stop() {
    const current = run.value;
    if (!current?.armed) return false;
    updateClock();
    await applyEvaluation({ run: { ...stopRun(current), stoppedClockDisplay: live.clockDisplay }, crossings: live.crossings, laps: live.laps });
    return true;
  }

  async function classify(classification, mode = useSettingsStore().state.mode, note = "") {
    if (starting.value || classifying.value || useHistoryStore().importing || useHistoryStore().clearing || (classification !== null && !RESULT_CLASSIFICATIONS.includes(classification))) return false;
    if (classification === null && !run.value?.classification) return true;
    classifying.value = true;
    try {
      const current = run.value || createClassifiedRun({ mode, note, classification,
        cursor: eventLog.getLastSeq(), durable: useDeviceStore().durable });
      const next = { ...current, classification, armed: false, closed: true };
      await applyEvaluation({ run: next, crossings: live.crossings, laps: live.laps });
      // Clearing an untimed classification removes its row; this attempt can be classified again.
      if (classification === null && run.value?.runId === next.runId &&
          !useHistoryStore().rows.some((row) => row.runId === next.runId)) {
        setRun({ ...run.value, historyId: null });
      }
      return true;
    } catch (error) {
      notyf.error(error.message || "Could not save classification");
      return false;
    } finally {
      classifying.value = false;
    }
  }

  function reset() {
    resetLive();
    return setRun(null);
  }

  function discardRecords() {
    for (const timer of freezes.values()) clearTimeout(timer);
    freezes.clear();
    generation = runLog.epoch();
    run.value = null;
    restored = true;
    resetLive();
  }

  function pendingRun(current) {
    if (!current || current.calibration || !(current.closed || (current.mode === "sprint" && current.verification === "verified"))) return current;
    if (!resultTicks(current).length || current.finalizeAt != null) return current;
    const device = useDeviceStore();
    return { ...current, finalizeAt: Date.now() + CALIBRATION_WAIT_MS,
      calibrationEdges: runLog.clone(device.ppsBootId === current.masterBootId ? device.ppsEdges : []) };
  }

  async function freezeNow(runId) {
    if (generation !== runLog.epoch()) return;
    clearTimeout(freezes.get(runId));
    freezes.delete(runId);
    const current = runLog.get(runId);
    if (!current || current.deleted || current.calibration || current.finalizeAt == null) return;
    const ticks = resultTicks(current);
    const calibration = freezeCalibration(current.calibrationEdges || [], ticks);
    let ns = null;
    if (current.verification === "verified") {
      if (current.mode === "sprint") ns = durationNs(calibration.points, current.startTick, current.finishTick);
      else if (current.totalValid && ticks.length >= 2) ns = durationNs(calibration.points, ticks[0], ticks[ticks.length - 1]);
    }
    const next = { ...current, calibration, durationNs: ns == null ? null : String(ns) };
    delete next.calibrationEdges;
    delete next.finalizeAt;
    if (run.value?.runId === runId) {
      await setRun(next);
      if (run.value?.runId === runId && !next.armed) showStoppedClock();
    } else await runLog.save(next);
    await useHistoryStore().updateCalibration(next);
  }

  function scheduleFreeze(current = run.value) {
    if (!current || current.calibration || current.finalizeAt == null || runLog.isDeleted(current.runId)) return;
    const ticks = resultTicks(current);
    const lastTick = ticks.reduce((m, t) => BigInt(t) > m ? BigInt(t) : m, 0n);
    if (Date.now() >= current.finalizeAt || current.calibrationEdges?.some(e => BigInt(e.tick) > lastTick)) {
      return freezeNow(current.runId);
    }
    if (!freezes.has(current.runId)) {
      freezes.set(current.runId, setTimeout(() => freezeNow(current.runId), current.finalizeAt - Date.now()));
    }
  }

  async function onPpsEdge(edge) {
    for (const current of runLog.all()) {
      if (current.calibration || current.finalizeAt == null || edge.master_boot_id !== current.masterBootId) continue;
      if (Date.now() > current.finalizeAt) { await freezeNow(current.runId); continue; }
      const edges = current.calibrationEdges || [];
      if (!edges.some(e => e.tick === edge.tick)) edges.push(runLog.clone(edge));
      const next = { ...current, calibrationEdges: edges };
      if (run.value?.runId === next.runId) await setRun(next);
      else await runLog.save(next);
      await scheduleFreeze(next);
    }
  }

  // Export waits only for already pending finalizations, never for a running race.
  async function settleResults() {
    const pending = runLog.all().filter(r => r.finalizeAt != null && !r.calibration);
    await Promise.all(pending.map(async r => {
      await new Promise(resolve => setTimeout(resolve, Math.max(0, r.finalizeAt - Date.now())));
      await freezeNow(r.runId);
    }));
    await runLog.flush();
  }

  function applyEvaluation(result) {
    const next = pendingRun({ ...result.run, throughSeq: eventLog.getLastSeq() });
    const before = run.value;
    live.crossings = result.crossings;
    live.laps = result.laps;
    const firstStart = result.crossings.find((c) => c.role === "start");
    if (firstStart && live.startedAt == null && next.armed) {
      live.startedAt = useDeviceStore().tickToWallMs(firstStart.tick);
      startClock();
    }
    const persisted = setRun(next);
    if (next.closed || !next.armed) {
      stopClock();
      showStoppedClock();
    }
    const changed =
      !before ||
      before.runId !== next.runId ||
      before.verification !== next.verification ||
      before.classification !== next.classification ||
      before.closed !== next.closed ||
      before.armed !== next.armed ||
      before.startTick !== next.startTick ||
      before.finishTick !== next.finishTick ||
      (before.crossingTicks || []).length !== next.crossingTicks.length;
    const saved = changed ? syncHistory(next, { throughSeq: eventLog.getLastSeq() }) : Promise.resolve();
    if ((next.closed && !(before?.closed && before.runId === next.runId)) ||
        (next.mode === "sprint" && next.verification === "verified" && before?.verification !== "verified")) scheduleFreeze();
    return Promise.all([persisted, saved]);
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
    syncHistory(run.value, {});
  }

  // After the event log is loaded: rebuild the live view of a restored run.
  async function reevaluate() {
    if (!restored) {
      restored = true;
      if (runLog.hasActiveState()) {
        const stored = runLog.active();
        run.value = stored && !stored.deleted ? decodeRun(encodeRun(stored)) : null;
      }
      await persist();
      for (const stored of runLog.all()) {
        if (stored.finalizeAt == null || stored.calibration) continue;
        const diskEdges = await ppsLog.loadEdges(stored.masterBootId);
        const edges = new Map((stored.calibrationEdges || []).map(e => [e.tick, e]));
        for (const e of diskEdges) if (e.received_at <= stored.finalizeAt) edges.set(e.tick, e);
        const next = { ...stored, calibrationEdges: [...edges.values()] };
        if (run.value?.runId === next.runId) await setRun(next);
        else await runLog.save(next);
        await scheduleFreeze(next);
      }
    }
    let current = pendingRun(run.value);
    if (current !== run.value) await setRun(current);
    resetLive();
    if (!current) return;
    const device = useDeviceStore();
    if (current.masterBootId != null && !device.connected) await device.useEdgesOf(current.masterBootId);
    await syncHistory(current, {});
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
      showStoppedClock();
      scheduleFreeze();
      return;
    }
    // A restored run cannot show a live wall clock (the wall/tick anchor is gone).
    evaluateOpen(current);
    scheduleFreeze();
    stopClock();
    live.startedAt = null;
    showStoppedClock();
  }

  return {
    run,
    live,
    starting,
    classifying,
    armed,
    fault,
    lightColor,
    resultNs,
    lapRows,
    crossingRows,
    reevaluate,
    start,
    stop,
    classify,
    reset,
    discardRecords,
    onEventRows,
    onMasterBoot,
    onMasterVersion,
    onPpsEdge,
    markNonDurable,
    qualityFor,
    missingRoles,
    settleResults,
  };
});
