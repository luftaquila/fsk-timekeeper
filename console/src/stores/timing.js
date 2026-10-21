/* The single run: START sequence, evidence evaluation, calibration freeze, live elapsed clock.
 * The run itself is the history record (runLog); History lists it once isRecord(). */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { useDeviceStore } from "./device";
import { useSettingsStore } from "./settings";
import { useHistoryStore } from "./history";
import * as eventLog from "../lib/eventLog";
import { createRun, createClassifiedRun, evaluateRun, stopRun, endMasterSession, shouldInvalidateOnMasterBoot, runTouchedBy, EngineError, MASTER_REBOOTED } from "../lib/engine";
import { wirelessQuality, missingRoles as missingRolesOf } from "../lib/quality";
import { encodeRun, decodeRun } from "../lib/run-codec";
import * as runLog from "../lib/runLog";
import { MODES, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { buildTimeline, freezeCalibration, utcMsAt } from "../lib/calibration";
import { resultTicks, resultNs as resultNsOf, lapsNs as lapsNsOf, spanNs, frozenDurationNs } from "../lib/results";
import { formatDuration } from "../lib/format";

const VERSION_CHANGED = "The master reports another protocol version.";

function makeLive() {
  return {
    clockDisplay: "00:00.000",
    raf: null,
    startedAt: null, // wall ms of the first start crossing (elapsed-clock display only)
    crossings: [], // engine crossings: { node_id, role, tick }
    laps: [], // engine laps: { startTick, endTick }
  };
}

export const useTimingStore = defineStore("timing", () => {
  const notyf = useNotification();
  const run = ref(null);
  const live = reactive(makeLive());
  const starting = ref(false);
  const classifying = ref(false);
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
    if (r.closed) return lapsNsOf(r, timeline.value).map((ns) => ({ ns }));
    return live.laps.map((lap) => ({ ns: spanNs(r, lap.startTick, lap.endTick, timeline.value) }));
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

  // DNS/DNF/DSQ (null clears) on the selected run, on another run by runId, or without any run on a
  // new untimed one. Classifying the selected run ends it.
  async function classify(classification, { mode, note = "", runId = run.value?.runId ?? null } = {}) {
    if (starting.value || classifying.value || useHistoryStore().importing || useHistoryStore().clearing || (classification !== null && !RESULT_CLASSIFICATIONS.includes(classification))) return false;
    const selected = runId === (run.value?.runId ?? null);
    const current = selected ? run.value : runLog.get(runId);
    if (!selected && (!current || current.deleted)) {
      notyf.error("Record not found");
      return false;
    }
    if (classification === null && !current?.classification) return true;
    classifying.value = true;
    try {
      const next = { ...(current || createClassifiedRun({ mode: mode ?? useSettingsStore().state.mode, note, classification,
        cursor: eventLog.getLastSeq(), durable: useDeviceStore().durable })), classification, armed: false, closed: true };
      if (selected) await applyEvaluation({ run: next, crossings: live.crossings, laps: live.laps });
      else await runLog.save(next);
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
    run.value = null;
    restored = true;
    resetLive();
  }

  // A decided result (sprint arrival, closed run) freezes its calibration at once from the PPS
  // edges at hand; once frozen, durationNs follows the current result.
  function withCalibration(current) {
    if (current?.calibration) {
      const ns = frozenDurationNs(current);
      return ns === current.durationNs ? current : { ...current, durationNs: ns };
    }
    if (!current || !(current.closed || (current.mode === "sprint" && current.verification === "verified"))) return current;
    if (!resultTicks(current).length) return current;
    const device = useDeviceStore();
    const edges = device.ppsBootId === current.masterBootId ? device.ppsEdges : [];
    const next = { ...current, calibration: freezeCalibration(edges, resultTicks(current)) };
    return { ...next, durationNs: frozenDurationNs(next) };
  }

  function applyEvaluation(result) {
    const next = withCalibration({ ...result.run, throughSeq: eventLog.getLastSeq() });
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
    return persisted;
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

  // The master's timebase is gone: an open run keeps the crossings already received and nothing more.
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

  // Storage failed: the run is no longer durable.
  function markNonDurable() {
    const current = run.value;
    if (!current || current.durable === false) return;
    setRun({ ...current, durable: false });
  }

  // A restore saved the selected run again: follow the run log's durable state.
  function markRestored() {
    const current = run.value;
    if (!current || current.durable !== false || !runLog.get(current.runId)?.durable) return;
    setRun({ ...current, durable: true });
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
    }
    const current = run.value;
    resetLive();
    if (!current) return;
    const device = useDeviceStore();
    if (current.masterBootId != null && !device.connected) await device.useEdgesOf(current.masterBootId);
    if (current.closed) {
      if (current.mode === "sprint") {
        live.crossings = [
          ...(current.startTick ? [{ node_id: "", role: "start", tick: current.startTick }] : []),
          ...(current.finishTick ? [{ node_id: "", role: "finish", tick: current.finishTick }] : []),
        ];
      } else {
        const c = current.crossingTicks || [];
        live.crossings = c.map((tick) => ({ node_id: "", role: "start", tick }));
        live.laps = c.slice(1).map((tick, i) => ({ startTick: c[i], endTick: tick }));
      }
      showStoppedClock();
      return;
    }
    // A restored run cannot show a live wall clock (the wall/tick anchor is gone).
    evaluateOpen(current);
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
    markNonDurable,
    markRestored,
    qualityFor,
    missingRoles,
  };
});
