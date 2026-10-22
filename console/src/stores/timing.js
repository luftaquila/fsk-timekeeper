/* The single run: START sequence, evidence evaluation, calibration freeze, live elapsed clock.
 * The run itself is the history record (runLog); History lists it once isRecord(). Camera modes
 * use the same run with the operator's decisions; a camera-laps run is one heat of several cars. */
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
import { MODES, CAMERA_MODES, MODE_KIND, RESULT_CLASSIFICATIONS } from "../lib/constants";
import { buildTimeline, freezeCalibration, extendCalibration, utcMsAt } from "../lib/calibration";
import { resultTicks, resultNs as resultNsOf, lapsNs as lapsNsOf, heldLaps, spanNs, frozenDurationNs, carRun } from "../lib/results";
import { crossingKey, mergeDecision } from "../lib/camera";
import { cameraNodeOf, bootIdText } from "../lib/protocol";
import { formatDuration } from "../lib/format";
import { useCameraStore } from "./camera";

const VERSION_CHANGED = "The master reports another protocol version.";
const CAMERA_LIVE_MS = 5000; // a held camera session's report and first checkpoint
const CAMERA_POLL_MS = 100;

function makeLive() {
  return {
    clockDisplay: "00:00.000",
    raf: null,
    startedAt: null, // wall ms of the first start crossing (elapsed-clock display only)
    crossings: [], // engine crossings: { node_id, role, tick } (+ camera fields)
    laps: [], // engine laps: { startTick, endTick }; camera-laps: { [car]: laps }
  };
}

export const useTimingStore = defineStore("timing", () => {
  const notyf = useNotification();
  const run = ref(null);
  const live = reactive(makeLive());
  const starting = ref(false);
  const cameraStarting = ref(false);
  const classifying = ref(false);
  let restored = false;

  const armed = computed(() => !!run.value?.armed);
  // On the camera Pi the camera session runs while something holds it: an armed camera-mode run
  // (a camera-mode Start holds it while it waits) or an open live view, on the Pi itself.
  const runHoldsCamera = computed(() => !!run.value?.armed && run.value.mode.startsWith("camera-"));
  const holdingCamera = computed(() => cameraStarting.value || runHoldsCamera.value);
  const fault = computed(() => run.value?.fault ?? null);
  const lightColor = computed(() => (!run.value ? "grey" : run.value.armed ? "green" : "red"));

  // PPS timeline of the run's master boot (provisional calibration until frozen).
  const timeline = computed(() => {
    const device = useDeviceStore();
    const r = run.value;
    return buildTimeline(r && device.ppsBootId === r.masterBootId ? device.ppsEdges : []);
  });

  const resultNs = computed(() => (run.value && run.value.mode !== "camera-laps" ? resultNsOf(run.value, timeline.value) : null));
  // Display laps (engine order); ns from the frozen calibration or the provisional timeline.
  const lapRows = computed(() => {
    const r = run.value;
    if (!r || r.mode === "camera-laps") return [];
    if (r.closed) return lapsNsOf(r, timeline.value).map((ns) => ({ ns }));
    return live.laps.map((lap) => ({ ns: spanNs(r, lap.startTick, lap.endTick, timeline.value) }));
  });

  // Crossing list of a role with the time since the first start crossing.
  function crossingRows(role) {
    const r = run.value;
    const list = live.crossings.filter((c) => c.role === role);
    const first = live.crossings.find((c) => c.role === "start");
    if (!r || !first) return list.map((c) => ({ ...c, key: c.key ?? `${c.node_id}:${c.tick}`, ns: null }));
    return list.map((c) => ({ ...c, key: c.key ?? `${c.node_id}:${c.tick}`, ns: spanNs(r, first.tick, c.tick, timeline.value) }));
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

  // camera-laps: each car's result, laps and total (ns from its frozen calibration or the timeline).
  // held: an unassigned crossing may be the car's (its lap count and best lap wait for the operator);
  // heldLaps: the laps it falls in.
  const carRows = computed(() => {
    const r = run.value;
    if (r?.mode !== "camera-laps") return [];
    return Object.keys(r.results).map((car) => {
      const view = carRun(r, car);
      return { car, result: r.results[car], laps: lapsNsOf(view, timeline.value), heldLaps: heldLaps(view),
        held: view.heldTicks.length > 0, totalNs: resultNsOf(view, timeline.value) };
    });
  });

  // The camera node and the sensors a mode times with. The camera stands at the finish: it times
  // the finish of camera-sprint, whose start is the sensor mapped to Start, and every camera-laps
  // lap (laps count start-role crossings).
  function mappingsFor(mode) {
    const settings = useSettingsStore();
    if (!mode.startsWith("camera-")) return settings.mappingsFor(mode);
    const camera = cameraNodeOf(useDeviceStore().identity);
    const cameraRole = mode === "camera-laps" ? "start" : "finish";
    const sensors = mode === "camera-laps" ? [] : settings.mappingsFor("sprint").filter((m) => m.role === "start" && m.node_id !== camera);
    return [...(camera ? [{ node_id: camera, role: cameraRole, label: "Camera" }] : []), ...sensors];
  }

  // Photo modes are listed once a camera is set up (Camera Host); whether it is live enough to
  // start is the Start check's (qualityFor).
  const modes = computed(() => MODES.filter((m) => !CAMERA_MODES.includes(m) || !!useSettingsStore().state.cameraHost));
  // The mode Timing shows and Start uses: the run's, else the chosen one while it is offered.
  const mode = computed(() => {
    const chosen = useSettingsStore().state.mode;
    return run.value?.mode ?? (modes.value.includes(chosen) ? chosen : MODES[0]);
  });

  function qualityFor(mode, now = Date.now()) {
    const device = useDeviceStore();
    return wirelessQuality({
      mode,
      mappings: mappingsFor(mode),
      telemetry: device.telemetry,
      masterFresh: device.masterFresh(now),
      connected: device.connected,
      contract: device.contract,
      pipeline: device.pipelineHealth(now),
      standby: cameraStandby(mode),
      now,
    });
  }

  // Until a camera-mode Start holds it, the camera session is judged by whether it can start: the
  // camera online at its Pi.
  function cameraStandby(mode) {
    const camera = useCameraStore();
    const node = cameraNodeOf(useDeviceStore().identity);
    if (!mode.startsWith("camera-") || !camera.connected || holdingCamera.value || !node) return {};
    return { [node]: !camera.contract.ok ? camera.contract.reason : camera.info.camera?.connected ? null : "Camera offline" };
  }

  // A camera-mode Start on the camera Pi holds the camera session, which the Pi starts then (its
  // background learnt now), and waits until that session is live: the Start check passes on its
  // report and its first checkpoint (sent once the background is learnt) is in. The last check
  // if it is not in time.
  async function openCamera(mode) {
    const device = useDeviceStore();
    const node = cameraNodeOf(device.identity);
    cameraStarting.value = true;
    const session = bootIdText(String((await useCameraStore().holdSession(true)).session));
    if (session == null) throw new Error("Camera session did not start");
    const deadline = Date.now() + CAMERA_LIVE_MS;
    for (;;) {
      const q = qualityFor(mode);
      const live = device.telemetry[node]?.sensor_boot_id === session &&
        eventLog.latestCheckpoint(node, device.masterBootId)?.sensor_boot_id === session;
      if ((q.ok && live) || Date.now() >= deadline) return q;
      await new Promise((resolve) => setTimeout(resolve, CAMERA_POLL_MS));
    }
  }

  function missingRoles(mode) {
    return missingRolesOf(mode, mappingsFor(mode));
  }

  // START: quality -> camera modes on the camera Pi: camera session held and live -> fresh master
  // clock -> quality again -> run from checkpoints -> catch up.
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
      if (q.ok && mode.startsWith("camera-") && useCameraStore().connected) q = await openCamera(mode);
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
      const camera = mode.startsWith("camera-") ? cameraNodeOf(device.identity) : null;
      const next = createRun({
        mode,
        note,
        clock,
        mappings: q.mappings,
        findCheckpoint: (node, boot, maxTick) => eventLog.latestCheckpoint(node, boot, maxTick),
        currentSensorBoot: (node) => device.telemetry[node]?.sensor_boot_id ?? null,
        lastSeq: cursorBefore,
        lapTarget: MODE_KIND[mode] === "laps" ? settings.state.lapTarget : null,
        debounceMs: settings.state.debounceMs,
        cameraNode: camera,
        ocrMinConf: settings.state.ocrMinConf,
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
      // A camera session held for a Start that did not arm is let go.
      if (cameraStarting.value && !run.value?.armed) useCameraStore().holdSession(false).catch(() => {});
      cameraStarting.value = false;
    }
  }

  // Like the FSK service, Stop closes the current snapshot without assigning DNS/DNF. A heat is
  // evaluated once more: its unassigned crossings are no longer pending.
  async function stop() {
    const current = run.value;
    if (!current?.armed) return false;
    updateClock();
    const stopped = { ...stopRun(current), stoppedClockDisplay: live.clockDisplay };
    await applyEvaluation(current.mode === "camera-laps" ? evaluateRun(stopped, runRows(stopped)) :
      { run: stopped, crossings: live.crossings, laps: live.laps });
    return true;
  }

  // Evidence rows of a run: since its cursor, through its last evaluation once closed.
  function runRows(current) {
    const rows = eventLog.since(current.cursor);
    return current.closed && current.throughSeq != null ? rows.filter((row) => row.seq <= current.throughSeq) : rows;
  }

  // An operator decision on a camera crossing of the selected run (open or closed); the run is
  // evaluated again from its evidence.
  async function decide(key, patch) {
    const current = run.value;
    if (!current?.mode.startsWith("camera-") || starting.value || classifying.value) return false;
    const rows = runRows(current);
    const row = rows.find((r) => r.kind === "capture" && r.camera && crossingKey(r) === key);
    try {
      if (!row) throw new Error("Crossing not found");
      const decision = mergeDecision(row, current.decisions?.[key], patch);
      const next = { ...current, decisions: { ...current.decisions, [key]: decision } };
      const evaluated = evaluateRun(next, rows);
      if (current.closed) evaluated.run = { ...evaluated.run, armed: false, closed: true, throughSeq: current.throughSeq };
      await applyEvaluation(evaluated, { keepThrough: current.closed });
      return true;
    } catch (error) {
      notyf.error(error.message || "Could not save the decision");
      return false;
    }
  }

  // DNS/DNF/DSQ (null clears) on the selected run, on another run by runId, or without any run on a
  // new untimed one. Classifying the selected run ends it.
  async function classify(classification, { mode, note = "", runId = run.value?.runId ?? null, car = null } = {}) {
    if (starting.value || classifying.value || useHistoryStore().importing || useHistoryStore().clearing || (classification !== null && !RESULT_CLASSIFICATIONS.includes(classification))) return false;
    const selected = runId === (run.value?.runId ?? null);
    const current = selected ? run.value : runLog.get(runId);
    if (!selected && (!current || current.deleted)) {
      notyf.error("Record not found");
      return false;
    }
    if (current?.mode === "camera-laps") return classifyCar(current, car, classification, selected);
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

  // One car of a heat; the heat closes once every car is decided.
  async function classifyCar(current, car, classification, selected) {
    if (!current.results?.[car]) {
      notyf.error("Car not found");
      return false;
    }
    if (classification === null && !current.results[car].classification) return true;
    classifying.value = true;
    try {
      const next = { ...current, results: { ...current.results, [car]: { ...current.results[car], classification } } };
      if (selected) {
        const evaluated = evaluateRun(next, runRows(current));
        if (current.closed) evaluated.run = { ...evaluated.run, armed: false, closed: true, throughSeq: current.throughSeq };
        await applyEvaluation(evaluated, { keepThrough: current.closed });
      } else await runLog.save(next);
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

  // A decided result (sprint arrival, closed run, decided car) freezes its calibration at once from
  // the PPS edges at hand; once frozen, durationNs follows the current result, and ticks a later
  // operator decision brings in take their time from the frozen points.
  function frozen(current, decided) {
    if (current.calibration) {
      const calibration = extendCalibration(current.calibration, resultTicks(current));
      const next = calibration === current.calibration ? current : { ...current, calibration };
      const ns = frozenDurationNs(next);
      return ns === next.durationNs ? next : { ...next, durationNs: ns };
    }
    if (!decided || !resultTicks(current).length) return current;
    const device = useDeviceStore();
    const edges = device.ppsBootId === current.masterBootId ? device.ppsEdges : [];
    const next = { ...current, calibration: freezeCalibration(edges, resultTicks(current)) };
    return { ...next, durationNs: frozenDurationNs(next) };
  }

  function withCalibration(current) {
    if (!current) return current;
    if (current.mode !== "camera-laps") {
      return frozen(current, current.closed || (MODE_KIND[current.mode] === "sprint" && current.verification === "verified"));
    }
    let changed = false;
    const results = {};
    for (const car of Object.keys(current.results)) {
      const before = carRun(current, car);
      const after = frozen(before, before.closed);
      const result = current.results[car];
      results[car] = after === before ? result : { ...result, calibration: after.calibration, durationNs: after.durationNs };
      changed ||= after !== before;
    }
    return changed ? { ...current, results } : current;
  }

  // A camera run keeps its crossings' camera records, so History can show them without evidence.
  function withCameraCrossings(next, crossings) {
    if (!next?.mode?.startsWith("camera-")) return next;
    const cameraCrossings = crossings.filter((c) => c.key).map(({ key, role, tick, autoTick, camera, car = null, state = "assigned" }) =>
      ({ key, role, tick, autoTick, camera, car, state }));
    return { ...next, cameraCrossings };
  }

  function applyEvaluation(result, { keepThrough = false } = {}) {
    const next = withCameraCrossings(withCalibration(keepThrough ? result.run : { ...result.run, throughSeq: eventLog.getLastSeq() }), result.crossings);
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
    if (current.closed && current.mode.startsWith("camera-")) {
      // The viewer needs the crossings' camera records: rebuild them from the run's evidence.
      const result = evaluateRun(current, runRows(current));
      live.crossings = result.crossings;
      live.laps = result.laps;
      showStoppedClock();
      return;
    }
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
    runHoldsCamera,
    holdingCamera,
    fault,
    lightColor,
    resultNs,
    timeline,
    lapRows,
    carRows,
    crossingRows,
    mappingsFor,
    modes,
    mode,
    decide,
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
