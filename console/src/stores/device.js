/* Master link: transport, contract check, line dispatch, evidence ingest + ack, telemetry,
 * PPS edges, clock reads, provisioning, device log, DFU. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { createSerialTransport, isSerialSupported } from "../transport/serial";
import { createFakeTransport } from "../transport/fake";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, formatAck, isHexKey, isRadioConfig, radioCommand, contractOf, resetNames, errorNames, RADIO_FAULT_MASK, RADIO_HISTORY_LABELS, RADIO_RECOVERY_REASONS, RADIO_RECOVERY_RESULTS, NODE_MASTER } from "../lib/protocol";
import { createWirelessClock } from "../lib/wireless-clock";
import { WIRELESS_STATUS_MAX_AGE_MS, UNREADABLE_ALARM_REPEATS, LOG_RETENTION_MS } from "../lib/constants";
import { MASTER_TICKS_PER_MS as TICKS_PER_MS } from "../lib/event-timing";
import { pipelineHealth as pipelineHealthOf } from "../lib/quality";
import * as eventLog from "../lib/eventLog";
import * as ppsLog from "../lib/ppsLog";
import * as deviceLog from "../lib/deviceLog";
import { useTimingStore } from "./timing";
import { useSettingsStore } from "./settings";

const CONSOLE_LIMIT = 300;
const LOG_VIEW_LIMIT = 500;
const PROVISION_TIMEOUT_MS = 3000;
const GPS_REPORT_MAX_AGE_MS = 3000;

export const useDeviceStore = defineStore("device", () => {
  const notyf = useNotification();

  const connected = ref(false);
  const connecting = ref(false);
  const transportKind = ref(null); // "serial" | "fake"
  const identity = ref(null); // parsed I line
  const radioKey = ref(null); // memory only: null = unread, "" = unprovisioned
  const keyReadState = ref("idle");
  let keyReadTimer = null;
  const contract = ref(contractOf(null)); // { ok, reason }
  const identityOk = computed(() => contract.value.ok);
  const lastLineAt = ref(0);
  const heartbeat = ref(null); // { tick: bigint, wallMs, uptimeMs, beaconSeq, nseen }
  const telemetry = reactive({}); // node_id -> normalized telemetry
  const masterBootId = ref(null);
  const pps = ref(null); // latest P report
  const ppsEdges = ref([]); // qualified PPS edges of ppsBootId, sorted by tick
  const ppsBootId = ref(null);
  const unprovisioned = ref(false);
  const dropped = reactive({ count: 0, reasons: {} });
  const stats = reactive({ lines: 0, events: 0, duplicates: 0, acks: 0, quarantined: 0, gated: 0 });
  const consoleLines = ref([]);
  const dfuInProgress = ref(false);
  const durable = ref(true);
  const durableError = ref(null);
  const storageWarning = ref(false); // navigator.storage.persist() refused
  const alarms = ref([]); // fatal conditions: { key, text, at }
  const quarantine = ref(null); // latest quarantined line: { count, reason, node, at }
  const verDrop = ref(null); // { count, at } once D 0 ver_drop rose
  const head = ref(null); // { hseq, firstSeenAt, lastSeenAt } of the latest E line
  const radioHistory = new Map(); // cumulative checkpoint snapshots, keyed by sensor boot
  let radioLogChain = Promise.resolve();
  const radioReports = new Map(); // latest snapshot per board, restored from the device log
  const logEntries = ref([]); // device log, newest first

  let transport = null;
  let wakeLock = null;
  let provisionPending = null; // { cmd, fails, resolve, timer }: K or R awaiting A <cmd> / X <fail>
  let ackChain = Promise.resolve();
  let lastVerDrop = null;
  let identityKey = null;
  const repeats = new Map(); // unreadable line key -> count

  const clock = createWirelessClock({
    send: ({ request_id }) => {
      if (!transport?.connected) throw new Error("Master not connected");
      transmitLine(`T ${request_id}`);
    },
  });

  const supported = computed(() => isSerialSupported());

  eventLog.onWriteFailure((error) => onStorageFailure(error));

  function masterFresh(now = Date.now()) {
    return connected.value && now - lastLineAt.value <= WIRELESS_STATUS_MAX_AGE_MS;
  }

  function pipelineHealth(now = Date.now()) {
    return pipelineHealthOf({ master: telemetry[NODE_MASTER], head: head.value, connected: connected.value, now });
  }

  // Wall-clock estimate of a master tick from the latest heartbeat (display only).
  function tickToWallMs(tick) {
    const hb = heartbeat.value;
    if (!hb) return Date.now();
    return hb.wallMs + Number(BigInt(tick) - hb.tick) / Number(TICKS_PER_MS);
  }

  function redactLine(text) {
    const line = String(text);
    if (/^K(?:\s|$)/.test(line.trim())) return "K ****";
    return line.replace(/\b[0-9a-f]{64}\b/gi, "****");
  }

  function pushConsole(dir, text) {
    text = redactLine(text);
    const arr = consoleLines.value;
    arr.push({ t: Date.now(), dir, text });
    if (arr.length > CONSOLE_LIMIT) arr.splice(0, arr.length - CONSOLE_LIMIT);
  }

  function transmitLine(line) {
    if (!transport) return Promise.resolve(false);
    pushConsole("tx", line);
    return transport.write(line);
  }

  function drop(reason, raw = null) {
    dropped.count++;
    dropped.reasons[reason] = (dropped.reasons[reason] || 0) + 1;
    if (raw != null) return log({ kind: "console", code: reason, text: raw });
  }

  async function log(entry) {
    const row = await deviceLog.addEntry({ at: Date.now(), devid: identity.value?.devid ?? null, masterBootId: masterBootId.value, ...entry, text: redactLine(entry.text) });
    logEntries.value = [row, ...logEntries.value].slice(0, LOG_VIEW_LIMIT);
    return row;
  }

  async function loadLog() {
    const entries = await deviceLog.listEntries();
    logEntries.value = entries.filter((e) => e.kind !== "state").slice(0, LOG_VIEW_LIMIT);
    const clearedThrough = Math.max(0, ...entries.filter((e) => e.code === "radio_history_clear").map((e) => e.throughSeq ?? 0));
    for (const e of [...entries].reverse()) {
      if (e.radioKey && e.radioSnapshot) radioReports.set(e.radioKey, e.radioSnapshot);
      if (e.historyKey && e.historySnapshot) radioHistory.set(e.historyKey, e.historySnapshot);
    }
    // Evidence is committed before ACK. Rebuild a missing display log if the
    // browser stopped between evidence storage and device-log creation.
    for (const row of eventLog.since(0)) {
      if (row.radio_history && row.seq > clearedThrough && row.received_at >= Date.now() - LOG_RETENTION_MS) {
        await logCheckpointHistory(row, row.received_at);
      }
    }
  }

  async function clearLog() {
    await deviceLog.clearEntries();
    // An intentional Clear must not be undone by the crash-recovery replay.
    await deviceLog.addEntry({ kind: "state", code: "radio_history_clear", at: Date.now(),
      throughSeq: eventLog.getLastSeq(), text: "" });
    logEntries.value = [];
    radioHistory.clear();
    radioReports.clear();
  }

  async function exportLog() {
    return (await deviceLog.listEntries()).filter((e) => e.kind !== "state");
  }

  function raiseAlarm(key, text) {
    if (alarms.value.some((a) => a.key === key)) return;
    alarms.value = [...alarms.value, { key, text, at: Date.now() }];
  }

  function dismissAlarm(key) {
    alarms.value = alarms.value.filter((a) => a.key !== key);
  }

  function syncDurability() {
    if (eventLog.isDurable() !== durable.value) {
      durable.value = eventLog.isDurable();
      durableError.value = eventLog.getInitError()?.message || null;
    }
  }

  function onStorageFailure(error) {
    durable.value = false;
    durableError.value = error?.message || String(error);
    log({ kind: "console", code: "storage_failure", text: durableError.value });
    useTimingStore().markNonDurable();
  }

  async function requestPersistence() {
    try {
      if (typeof navigator === "undefined" || !navigator.storage?.persist) return null;
      const granted = await navigator.storage.persist();
      storageWarning.value = !granted;
      return granted;
    } catch {
      return null;
    }
  }

  async function handleLine(raw) {
    const now = Date.now();
    lastLineAt.value = now;
    stats.lines++;
    const msg = parseLine(raw);
    if (!msg) return;
    if ((msg.type !== "H" && msg.type !== "P") || consoleLines.value.length < CONSOLE_LIMIT) pushConsole("rx", String(raw).trim());
    // Nothing is judged or acked before the master proved the contract; E lines come again.
    if (["E", "D", "T", "P"].includes(msg.type) && !contract.value.ok) {
      stats.gated++;
      return;
    }
    switch (msg.type) {
      case "K":
        if (keyReadState.value === "loading") {
          finishKeyRead(msg.key === null ? "invalid" : msg.key ? "ready" : "empty", msg.key);
        }
        break;
      case "I":
        onIdentity(msg, String(raw).trim());
        break;
      case "H":
        if (/^\d+$/.test(msg.nowTick || "")) heartbeat.value = { tick: BigInt(msg.nowTick), wallMs: now, uptimeMs: msg.uptimeMs, beaconSeq: msg.beaconSeq, nseen: msg.nseen };
        break;
      case "D":
        await onTelemetry(msg.telemetry, now, raw);
        break;
      case "E":
        await handleEventLine(msg.raw, now);
        break;
      case "T":
        if (msg.masterBootId == null) break;
        clock.accept({ request_id: msg.requestId, master_tick: msg.masterTick, master_boot_id: msg.masterBootId });
        setMasterBoot(msg.masterBootId);
        break;
      case "P":
        await onPps(msg.pps, now, raw);
        break;
      case "A":
        if (msg.cmd === provisionPending?.cmd) resolveProvision("ok");
        break;
      case "X":
        onErrorLine(msg);
        break;
      default:
        await drop("unknown_line", raw);
    }
  }

  function onIdentity(msg, raw) {
    // The same boot answering ?ID again, or after R changed its radio settings.
    const key = msg.v1 ? raw : [msg.product, msg.usbProto, msg.fw, msg.devid, msg.role, msg.radioProto, msg.ticksPerMs, msg.resetReason].join(" ");
    const repeated = identityKey === key;
    identityKey = key;
    identity.value = msg;
    contract.value = contractOf(msg);
    const reason = Number.isInteger(msg.resetReason) ? resetNames(msg.resetReason).join(", ") : "unknown";
    if (!repeated) log({ kind: "boot", code: reason, text: raw, devid: msg.devid ?? null });
    useTimingStore().onMasterVersion({ usbProto: msg.usbProto, radioProto: msg.radioProto });
  }

  async function onTelemetry(raw, now, line) {
    const t = normalizeTelemetry(raw, now);
    if (!t) {
      return drop("tel.format", line);
    }
    telemetry[t.node_id] = t;
    const radioLog = radioLogChain = radioLogChain.then(() => logRadioHealth(t, now));
    if (t.node_id !== NODE_MASTER) return radioLog;
    if (t.provisioned === 1) unprovisioned.value = false;
    if (t.master_boot_id != null) setMasterBoot(t.master_boot_id);
    if (t.ver_drop != null) {
      if (lastVerDrop != null && t.ver_drop > lastVerDrop) verDrop.value = { count: t.ver_drop, at: now };
      lastVerDrop = t.ver_drop;
    }
    await radioLog;
  }

  async function logRadioHealth(t, now) {
    const boot = t.node_id === NODE_MASTER ? t.master_boot_id : t.sensor_boot_id;
    if (boot == null) return;
    const board = t.node_id === NODE_MASTER ? identity.value?.devid ?? "unknown" : t.node_id;
    const key = `${board}:${boot}`;
    const prior = radioReports.get(key);
    const previous = prior?.boot === boot ? prior : null;
    // Sensor history arrives only in acknowledged checkpoints, never in cached D lines.
    const recovery = t.node_id === NODE_MASTER ? t.recovery : null;
    const snapshot = { boot, ...recovery, faults: (t.err_flags ?? 0) & RADIO_FAULT_MASK };
    radioReports.set(key, snapshot); // set before the first await: repeated D lines cannot duplicate it
    if (radioReports.size > 512) radioReports.delete(radioReports.keys().next().value);
    const entry = { kind: "radio", at: now, devid: t.node_id === NODE_MASTER ? identity.value?.devid : t.node_id,
      masterBootId: t.master_boot_id, nodeId: t.node_id, nodeBootId: boot, radioKey: key, radioSnapshot: snapshot };
    const label = t.node_id === NODE_MASTER ? "Master" : `Sensor ${t.node_id}`;
    if (snapshot.count > 0 && (!previous || ["count", "failures", "reason", "result"].some((k) => snapshot[k] !== previous[k]))) {
      await log({ ...entry, code: "radio_recovery", text: `${label}: radio reinitializations ${snapshot.count}${snapshot.count === 65535 ? "+" : ""}, failed ${snapshot.failures}${snapshot.failures === 65535 ? "+" : ""}; latest reason: ${RADIO_RECOVERY_REASONS[snapshot.reason]}; latest result: ${RADIO_RECOVERY_RESULTS[snapshot.result]}.` });
    }
    const raised = snapshot.faults & ~(previous?.faults ?? 0);
    const cleared = (previous?.faults ?? 0) & ~snapshot.faults;
    if (raised) await log({ ...entry, code: "radio_fault", text: `${label}: ${errorNames(raised).join(", ")}.` });
    if (cleared) await log({ ...entry, code: "radio_restored", text: `${label}: cleared ${errorNames(cleared).join(", ")}.` });
  }

  async function logCheckpointHistory(row, now) {
    const historyKey = `${row.node_id}:${row.sensor_boot_id}`;
    const historySnapshot = { ...row.radio_history, evSeq: row.ev_seq };
    const prior = radioHistory.get(historyKey);
    if (prior) {
      // Replayed checkpoints (including another master session) cannot roll a
      // per-boot cumulative total backwards. Equal totals are harmless.
      if (historySnapshot.faults.some((v, i) => v < prior.faults[i]) ||
          historySnapshot.recovery.count < prior.recovery.count ||
          historySnapshot.recovery.failures < prior.recovery.failures) return;
      const distance = (row.ev_seq - prior.evSeq + 65535) % 65535;
      const increased = historySnapshot.faults.some((v, i) => v > prior.faults[i]) ||
        historySnapshot.recovery.count > prior.recovery.count || historySnapshot.recovery.failures > prior.recovery.failures;
      if (distance > 32767 && !increased) return;
    }
    const changes = [];
    const total = (n) => `${n}${n === 65535 ? "+" : ""}`;
    for (let i = 0; i < historySnapshot.faults.length; i++) {
      const n = historySnapshot.faults[i];
      if (n > (prior?.faults[i] ?? 0)) changes.push(`${RADIO_HISTORY_LABELS[i]}: ${total(n)} since boot`);
    }
    const r = historySnapshot.recovery;
    if (r.count && (!prior || ["count", "failures", "reason", "result"].some((k) => r[k] !== prior.recovery[k]))) {
      changes.push(`radio reinitializations: ${total(r.count)}, failed: ${total(r.failures)}; latest reason: ${RADIO_RECOVERY_REASONS[r.reason]}; latest result: ${RADIO_RECOVERY_RESULTS[r.result]}`);
    }
    if (changes.length) {
      await log({ kind: "radio", code: "radio_history", at: now, devid: row.node_id,
        nodeId: row.node_id, nodeBootId: row.sensor_boot_id, masterBootId: row.master_boot_id,
        historyKey, historySnapshot, text: `Sensor ${row.node_id}: ${changes.join("; ")}. Reported at this time; event times are unavailable.` });
    }
    radioHistory.set(historyKey, historySnapshot);
  }

  function setMasterBoot(id) {
    if (masterBootId.value === id) return;
    if (masterBootId.value != null) pps.value = null;
    masterBootId.value = id;
    useTimingStore().onMasterBoot(id);
    if (ppsBootId.value !== id) useEdgesOf(id);
  }

  // Load the qualified PPS edges of a master boot (restore / reconnect / new boot).
  async function useEdgesOf(bootId) {
    if (bootId == null || ppsBootId.value === bootId) return;
    ppsBootId.value = bootId;
    ppsEdges.value = [];
    const stored = await ppsLog.loadEdges(bootId);
    if (ppsBootId.value !== bootId) return;
    const merged = new Map(stored.map((e) => [e.tick, e]));
    for (const e of ppsEdges.value) merged.set(e.tick, e);
    ppsEdges.value = [...merged.values()].sort((a, b) => (BigInt(a.tick) < BigInt(b.tick) ? -1 : 1));
  }

  function onPps(raw, now, line) {
    const p = normalizePps(raw, now);
    if (!p) {
      return drop("pps", line);
    }
    pps.value = p;
    const bootId = masterBootId.value;
    if (!p.seg || bootId == null || ppsBootId.value !== bootId) return;
    if (ppsEdges.value.some((e) => e.tick === p.tick)) return;
    const edge = { master_boot_id: bootId, tick: p.tick, seg: p.seg, n: p.n, utc: p.utc, received_at: now };
    const list = ppsEdges.value;
    ppsEdges.value = list.length && BigInt(list[list.length - 1].tick) < BigInt(edge.tick) ? [...list, edge] : [...list, edge].sort((a, b) => (BigInt(a.tick) < BigInt(b.tick) ? -1 : 1));
    ppsLog.saveEdge(edge);
    useTimingStore().onPpsEdge(edge);
  }

  function onErrorLine(msg) {
    if (msg.reason === "badcmd" && keyReadState.value === "loading") {
      finishKeyRead("unsupported");
      return;
    }
    if (msg.reason === "noprov") {
      if (!unprovisioned.value) log({ kind: "X", code: msg.reason, text: msg.text });
      unprovisioned.value = true;
      return;
    }
    log({ kind: "X", code: msg.reason, text: msg.text });
    if (provisionPending?.fails.includes(msg.reason)) resolveProvision(msg.reason);
    else if (msg.reason === "clock") clock.close("The master rejected the clock request");
  }

  function noteHead(hseq, now) {
    if (head.value?.hseq === hseq) head.value = { ...head.value, lastSeenAt: now };
    else head.value = { hseq, firstSeenAt: now, lastSeenAt: now };
  }

  function noteUnreadable(key, raw, reason) {
    const n = (repeats.get(key) || 0) + 1;
    repeats.set(key, n);
    // Preserve the first rejection even if it never repeats enough to raise an alarm.
    drop(reason, n === 1 ? raw : null);
    if (n === UNREADABLE_ALARM_REPEATS) {
      raiseAlarm(`unreadable:${key}`, "The master sent an event the console cannot read.");
      log({ kind: "console", code: "unreadable_event", text: raw });
    }
  }

  async function handleEventLine(raw, now) {
    stats.events++;
    const p = parseEventLine(raw);
    if (p.stage === "unreadable") return noteUnreadable(`raw:${p.raw}`, p.raw, "event.unreadable");
    noteHead(p.hseq, now);
    if (p.stage === "crc") return noteUnreadable(`hseq:${p.hseq}`, p.raw, "event.crc");
    if (p.stage === "invalid" && p.masterBootId == null) return noteUnreadable(`hseq:${p.hseq}`, p.raw, "event.boot");
    // Serialized: store (one transaction), then ack, then evaluate.
    ackChain = ackChain
      .then(async () => {
        if (p.stage === "invalid") {
          const { duplicate, marker } = await eventLog.ingestQuarantine({ raw: p.raw, reason: p.reason, node: p.node, hseq: p.hseq, masterBootId: p.masterBootId }, now);
          syncDurability();
          if (await transmitLine(formatAck(p))) stats.acks++;
          if (duplicate) {
            stats.duplicates++;
            return;
          }
          stats.quarantined++;
          quarantine.value = { count: stats.quarantined, reason: p.reason, node: p.node, at: now };
          log({ kind: "console", code: "quarantine", text: `${p.reason}: ${p.raw}` });
          useTimingStore().onEventRows([marker]);
          return;
        }
        const { duplicate, rows } = await eventLog.ingestLine({ rows: p.rows, masterBootId: p.masterBootId, hseq: p.hseq }, now);
        syncDurability();
        // Also on duplicate E lines: a previous attempt may have stopped after
        // the evidence transaction but before the device log was materialized.
        for (const row of p.rows) {
          if (row.radio_history) await logCheckpointHistory(row, now);
        }
        // Only now may the master drop the line from its queue.
        if (await transmitLine(formatAck(p))) stats.acks++;
        if (duplicate) {
          stats.duplicates++;
          return;
        }
        useTimingStore().onEventRows(rows);
      })
      .catch((e) => notyf.error(`Event handling failed: ${e.message}`));
    await ackChain;
  }

  function resolveProvision(result) {
    const p = provisionPending;
    provisionPending = null;
    if (!p) return;
    clearTimeout(p.timer);
    p.resolve(result);
  }

  // One provisioning command to the connected board (any role); resolves "ok", one of fails, or "timeout".
  function provision(cmd, line, shown, fails) {
    if (!transport?.connected) return Promise.reject(new Error("Master not connected"));
    if (keyReadState.value === "loading") return Promise.reject(new Error("Wait for the board settings to load."));
    if (provisionPending) return Promise.reject(new Error("A settings update is already in progress"));
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolveProvision("timeout"), PROVISION_TIMEOUT_MS);
      provisionPending = { cmd, fails, resolve, timer };
      pushConsole("tx", shown);
      transport.write(line).then((sent) => {
        if (!sent) resolveProvision("timeout");
      });
    });
  }

  // Send the fleet key; resolves "ok" | "keyfail" | "timeout".
  async function provisionKey(hex) {
    if (!isHexKey(hex)) return Promise.reject(new Error("Enter 64 Hex Characters"));
    const boardTransport = transport;
    const result = await provision("K", `K ${hex}`, "K ****", ["keyfail"]);
    if (result === "ok" && transport === boardTransport) finishKeyRead("ready", hex.toLowerCase());
    return result;
  }

  function finishKeyRead(state, key = null) {
    clearTimeout(keyReadTimer);
    keyReadTimer = null;
    radioKey.value = key;
    keyReadState.value = state;
  }

  function readRadioKey() {
    if (!transport?.connected || provisionPending || keyReadState.value === "loading") return;
    const boardTransport = transport;
    finishKeyRead("loading");
    keyReadTimer = setTimeout(() => finishKeyRead("timeout"), PROVISION_TIMEOUT_MS);
    transmitLine("?KEY").then((sent) => {
      if (!sent && transport === boardTransport && keyReadState.value === "loading") finishKeyRead("timeout");
    }).catch(() => {
      if (transport === boardTransport && keyReadState.value === "loading") finishKeyRead("timeout");
    });
  }

  // "ok" means stored and applied; cfgapply/cfgpending mean stored but not running.
  // The fresh I line reports the stored target settings, even on application failure.
  function configureRadio(cfg) {
    if (!isRadioConfig(cfg)) return Promise.reject(new Error("Radio settings out of range."));
    const line = radioCommand(cfg);
    return provision("R", line, line, ["cfgfail", "cfgapply", "cfgpending", "badcmd"]);
  }

  function readClock() {
    return clock.read();
  }

  // Latest GPS report for the START snapshot (reference only), or null when stale.
  function gpsReport(now = Date.now()) {
    const p = pps.value;
    if (!p || now - p.at > GPS_REPORT_MAX_AGE_MS) return null;
    return { valid: p.valid, ppb: p.ppb, fix: p.fix, sats: p.sats, span: p.span, utc: p.utc, tick: p.tick };
  }

  /* Screen Wake Lock: keep the console awake while connected. */
  async function acquireWakeLock() {
    try {
      if ("wakeLock" in navigator && !wakeLock) wakeLock = await navigator.wakeLock.request("screen");
    } catch {
      /* optional */
    }
  }
  function releaseWakeLock() {
    try {
      wakeLock?.release();
    } catch {
      /* ignore */
    }
    wakeLock = null;
  }
  if (typeof document !== "undefined") {
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible" && connected.value) acquireWakeLock();
    });
  }

  function onDisconnect() {
    const wasDfu = dfuInProgress.value;
    finishDisconnect();
    if (wasDfu) notyf.success("Bootloader Requested");
    else notyf.error("The master was disconnected.");
  }

  function resetSession() {
    finishKeyRead("idle");
    identity.value = null;
    contract.value = contractOf(null);
    heartbeat.value = null;
    pps.value = null;
    head.value = null;
    unprovisioned.value = false;
    lastVerDrop = null;
    identityKey = null;
    repeats.clear();
  }

  function finishDisconnect() {
    if (transport?.kind === "fake") {
      const settings = useSettingsStore();
      for (const node of transport.sensors.keys()) {
        delete telemetry[node];
        settings.removeMapping(node);
      }
      delete telemetry[NODE_MASTER];
    }
    connected.value = false;
    connecting.value = false;
    dfuInProgress.value = false;
    resetSession();
    releaseWakeLock();
    clock.close("The master was disconnected.");
    if (provisionPending) resolveProvision("timeout");
    transport = null;
    transportKind.value = null;
  }

  function makeTransport(kind) {
    const callbacks = { onLine: handleLine, onDisconnect };
    return kind === "fake" ? createFakeTransport(callbacks) : createSerialTransport(callbacks);
  }

  // kind: "serial" (default) | "fake" (dev simulator).
  async function connect({ kind = "serial" } = {}) {
    if (connected.value || connecting.value) return true;
    if (kind === "serial" && !isSerialSupported()) {
      notyf.error("Web Serial is not supported by this browser");
      return false;
    }
    connecting.value = true;
    const t = makeTransport(kind);
    try {
      await t.open();
      transport = t;
      transportKind.value = kind;
      connected.value = true;
      connecting.value = false;
      resetSession();
      lastLineAt.value = Date.now();
      acquireWakeLock();
      transmitLine("?ID");
      transmitLine("?STATUS");
      readRadioKey();
      notyf.success(kind === "fake" ? "Fake master connected" : "Master connected");
      return true;
    } catch (e) {
      connecting.value = false;
      if (e?.name !== "NotFoundError") notyf.error(`Connection failed: ${e.message || e}`);
      return false;
    }
  }

  async function disconnect() {
    const t = transport;
    finishDisconnect();
    try {
      await t?.close();
    } catch {
      /* ignore */
    }
  }

  async function enterBootloader() {
    if (!transport) throw new Error("Master not connected");
    if (useTimingStore().armed) throw new Error("Stop the running measurement first.");
    dfuInProgress.value = true;
    const t = transport;
    try {
      await t.enterBootloader();
    } finally {
      if (transport === t) finishDisconnect();
    }
  }

  function fakeTransport() {
    return transportKind.value === "fake" ? transport : null;
  }

  return {
    supported,
    connected,
    connecting,
    transportKind,
    identity,
    radioKey,
    keyReadState,
    identityOk,
    contract,
    lastLineAt,
    heartbeat,
    telemetry,
    masterBootId,
    pps,
    ppsEdges,
    ppsBootId,
    unprovisioned,
    dropped,
    stats,
    consoleLines,
    dfuInProgress,
    durable,
    durableError,
    storageWarning,
    alarms,
    quarantine,
    verDrop,
    head,
    logEntries,
    masterFresh,
    pipelineHealth,
    tickToWallMs,
    connect,
    disconnect,
    transmitLine,
    readClock,
    gpsReport,
    useEdgesOf,
    provisionKey,
    readRadioKey,
    configureRadio,
    enterBootloader,
    fakeTransport,
    handleLine,
    requestPersistence,
    loadLog,
    clearLog,
    exportLog,
    dismissAlarm,
  };
});
