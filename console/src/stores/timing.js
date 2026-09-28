/* The single run: START sequence, evidence evaluation, live clock, view facade. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { useDeviceStore } from "./device";
import { useSettingsStore } from "./settings";
import { useHistoryStore } from "./history";
import * as eventLog from "../lib/eventLog";
import { createRun, evaluateRun, invalidateRun, stopRun, shouldInvalidateOnMasterBoot, runTouchedBy, EngineError } from "../lib/engine";
import { wirelessQuality, missingRoles as missingRolesOf } from "../lib/quality";
import { encodeRun, decodeRun } from "../lib/run-codec";
import { MODES } from "../lib/constants";
import { masterTickDistanceBelowMs, masterTickDurationsMs, tickDurationMs } from "../lib/event-timing";
import { CAPTURE_HEALTH } from "../lib/capture-integrity";
import { msToClockStr, tickDeltaToMs } from "../lib/format";

const KEY_RUN = "tk.run.v2";
const CP_RETRY_MS = 2000; // re-ask for checkpoints while a stopped run waits (a sensor may miss the beacons)

function makeLive() {
  return {
    clockDisplay: "00:00.000",
    raf: null,
    startedAt: null, // wall ms of the first start crossing (display only)
    crossings: [], // raw, debounced crossings: { key, node_id, role, tick, ms, receivedAt, confirmed }
    accepted: [], // engine-confirmed crossings
    throughTick: null,
    lastRawTick: {}, // node -> last raw accepted tick (debounce)
  };
}

export const useTimingStore = defineStore("timing", () => {
  const notyf = useNotification();
  const run = ref(null);
  const live = reactive(makeLive());
  const starting = ref(false);
  let cpTimer = null; // checkpoint requests for a stopped run awaiting confirmation

  const armed = computed(() => !!run.value?.armed);
  const fault = computed(() => run.value?.fault ?? null);
  const lightColor = computed(() => (!run.value ? "grey" : run.value.armed ? "green" : "red"));
  // HFXO correction frozen into the run (0 = nominal 16 MHz).
  const ppb = computed(() => run.value?.calib?.ppb ?? 0);
  const laps = computed(() => (run.value?.lapTicks || []).map((t) => masterTickDurationsMs([t], ppb.value)));
  // Laps derived from raw crossings (immediate, before verification).
  const rawLaps = computed(() => {
    const starts = live.crossings.filter((c) => c.role === "start");
    const out = [];
    for (let i = 1; i < starts.length; i++) {
      out.push({ ms: tickDeltaToMs(starts[i].tick, starts[i - 1].tick, ppb.value), confirmed: starts[i].confirmed && starts[i - 1].confirmed });
    }
    return out;
  });

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

  // Restore the open run before the event log loads so its cursor is protected.
  function restore() {
    try {
      const saved = decodeRun(localStorage.getItem(KEY_RUN));
      run.value = saved && MODES.includes(saved.mode) ? saved : null;
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
      if (live.startedAt != null) live.clockDisplay = msToClockStr(Date.now() - live.startedAt);
      live.raf = requestAnimationFrame(tick);
    };
    live.raf = requestAnimationFrame(tick);
  }
  function resetLive() {
    stopClock();
    Object.assign(live, makeLive());
  }

  // A stopped run closes when every sensor confirms through the stop tick; ask the master for
  // checkpoints now and every CP_RETRY_MS until it does (Stop itself makes no radio event).
  function stopCheckpointRequests() {
    if (cpTimer) clearInterval(cpTimer);
    cpTimer = null;
  }
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
    return wirelessQuality({ mode, mappings: settings.mappingsFor(mode), telemetry: device.telemetry, masterFresh: device.masterFresh(now), now });
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
      const calibration = device.ppsCalibration();
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
        calibration,
      });
      // Absolute start time from the PPS anchor (UTC second of the last PPS edge + tick offset).
      next.startedUtc = null;
      if (calibration?.utc) {
        const delta = BigInt(clock.master_tick) - BigInt(calibration.ppsTick);
        if (delta >= 0n && delta < 5n * 16_000_000n) {
          next.startedUtc = new Date(calibration.utc * 1000 + tickDurationMs(delta, calibration.ppb)).toISOString();
        }
      }
      stopCheckpointRequests();
      resetLive();
      settings.rememberNote(note);
      setRun(next);
      next.historyId = await history.open(next, { masterDevId: device.identity?.devid || null });
      persist();
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

  // Latest tick this run has evidence for (any row from its sensors under its master boot).
  function latestEvidenceTick(current) {
    let latest = BigInt(current.boundaryTick);
    for (const row of eventLog.since(current.cursor)) {
      if (row.master_boot_id !== current.masterBootId || !current.nodes[row.node_id]) continue;
      const t = BigInt(row.master_tick);
      if (t > latest) latest = t;
    }
    return latest;
  }

  // STOP: fence at the master's current tick. Crossings before it still count when they arrive
  // late; the run closes once every sensor confirms its evidence through the fence.
  async function stop() {
    const current = run.value;
    if (!current?.armed) return false;
    const device = useDeviceStore();
    let stopTick = null;
    if (device.connected) {
      try {
        const clock = await device.readClock();
        if (clock.master_boot_id === current.masterBootId) stopTick = BigInt(clock.master_tick);
      } catch {
        /* master unreachable: fence at the latest evidence instead */
      }
    }
    const latest = run.value;
    if (!latest || latest.runId !== current.runId || !latest.armed) return false;
    if (stopTick == null) stopTick = latestEvidenceTick(latest);
    applyEvaluation(evaluateRun(stopRun(latest, stopTick), eventLog.since(latest.cursor)));
    if (run.value && !run.value.closed) requestCheckpointsUntilClosed(latest.runId);
    return true;
  }

  function reset() {
    stopCheckpointRequests();
    resetLive();
    setRun(null);
  }

  // Raw crossing bookkeeping for the live view (immediate; the engine confirms later).
  function noteRawCrossing(row) {
    const current = run.value;
    if (row.flags !== CAPTURE_HEALTH) return;
    const source = current.nodes[row.node_id];
    if (!source) return;
    if (BigInt(row.master_tick) < BigInt(current.boundaryTick)) return;
    const last = live.lastRawTick[row.node_id];
    if (last != null && masterTickDistanceBelowMs(row.master_tick, last, current.debounceMs)) return;
    live.lastRawTick[row.node_id] = row.master_tick;
    const key = `${row.node_id}:${row.master_tick}`;
    if (live.crossings.some((c) => c.key === key)) return;
    if (source.role === "start" && live.startedAt == null) {
      live.startedAt = useDeviceStore().tickToWallMs(row.master_tick);
      startClock();
    }
    live.crossings.push({ key, node_id: row.node_id, role: source.role, tick: row.master_tick, ms: null, receivedAt: row.received_at, confirmed: false });
    live.crossings.sort((a, b) => (BigInt(a.tick) < BigInt(b.tick) ? -1 : BigInt(a.tick) > BigInt(b.tick) ? 1 : 0));
    const first = live.crossings.find((c) => c.role === "start");
    for (const c of live.crossings) c.ms = first ? tickDeltaToMs(c.tick, first.tick, ppb.value) : null;
  }

  function applyEvaluation(result) {
    const next = result.run;
    live.accepted = result.accepted;
    live.throughTick = result.throughTick;
    const confirmed = new Set(result.accepted.map((e) => `${e.node_id}:${e.master_tick}`));
    for (const c of live.crossings) c.confirmed = confirmed.has(c.key);
    if (result.complete || next.closed || !next.armed) {
      stopClock();
      if (next.result != null) live.clockDisplay = msToClockStr(next.result);
    } else if (next.result != null && live.startedAt == null) {
      live.clockDisplay = msToClockStr(next.result);
    }
    const before = run.value;
    setRun(next);
    if (next.closed) stopCheckpointRequests();
    const changed =
      !before ||
      before.result !== next.result ||
      before.verification !== next.verification ||
      before.closed !== next.closed ||
      before.armed !== next.armed ||
      before.lapTicks.length !== next.lapTicks.length;
    if (changed) useHistoryStore().upsert(next, { throughSeq: eventLog.getLastSeq() });
    if (next.fault && (!before || !before.fault)) notyf.error(next.fault.reasons?.[0]?.reason || "Measurement fault");
  }

  // New rows arrived (or the run was just opened): re-evaluate if they touch the open run.
  function onEventRows(rows) {
    const current = run.value;
    if (!rows.length || !current || current.closed) return;
    for (const row of rows) noteRawCrossing(row);
    if (!runTouchedBy(current, rows)) return;
    applyEvaluation(evaluateRun(current, eventLog.since(current.cursor)));
  }

  // Telemetry changed: an armed run whose quality degraded goes back to pending.
  function onTelemetry() {
    const current = run.value;
    if (!current || !current.armed || current.closed) return;
    if (qualityFor(current.mode).ok) return;
    if (current.verification !== "pending") setRun({ ...current, verification: "pending" });
  }

  function onMasterBoot(masterBootId) {
    const current = run.value;
    if (!shouldInvalidateOnMasterBoot(current, masterBootId)) return;
    const next = invalidateRun(current, [{ node_id: "0", reason: "The master rebooted during the run." }], { awaitEvidence: true });
    stopClock();
    setRun(next);
    useHistoryStore().upsert(next, { throughSeq: eventLog.getLastSeq() });
    notyf.error("The master rebooted during the run.");
  }

  // After the event log is loaded: rebuild live state and the result of a restored run.
  function reevaluate() {
    const current = run.value;
    resetLive();
    if (!current) return;
    const rows = eventLog.since(current.cursor);
    for (const row of rows) noteRawCrossing(row);
    // A restored run cannot show a live wall clock (the wall/tick anchor is gone).
    stopClock();
    live.startedAt = null;
    if (current.result != null) live.clockDisplay = msToClockStr(current.result);
    if (!current.closed && rows.length) applyEvaluation(evaluateRun(current, rows));
    const restored = run.value;
    if (restored && !restored.closed && restored.stopTick != null) requestCheckpointsUntilClosed(restored.runId);
  }

  const source = {
    get connected() {
      return useDeviceStore().connected;
    },
    get run() {
      return run.value;
    },
    get armed() {
      return armed.value;
    },
    get starting() {
      return starting.value;
    },
    get lightColor() {
      return lightColor.value;
    },
    get clockDisplay() {
      return live.clockDisplay;
    },
    get crossings() {
      return live.crossings;
    },
    get accepted() {
      return live.accepted;
    },
    get laps() {
      return laps.value;
    },
    get rawLaps() {
      return rawLaps.value;
    },
    get result() {
      return run.value?.result ?? null;
    },
    get verification() {
      return run.value?.verification ?? null;
    },
    get fault() {
      return fault.value;
    },
    get calib() {
      return run.value?.calib ?? null;
    },
    get quality() {
      return qualityFor(useSettingsStore().state.mode);
    },
    get missingRoles() {
      return missingRoles(useSettingsStore().state.mode);
    },
    start: (note) => start(useSettingsStore().state.mode, note),
    stop,
    reset,
  };

  return {
    run,
    live,
    starting,
    armed,
    fault,
    lightColor,
    laps,
    rawLaps,
    restore,
    reevaluate,
    start,
    stop,
    reset,
    onEventRows,
    onTelemetry,
    onMasterBoot,
    qualityFor,
    missingRoles,
    source,
  };
});
