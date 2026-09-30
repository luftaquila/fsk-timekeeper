/* Master link: transport, contract check, line dispatch, evidence ingest + ack, telemetry,
 * PPS edges, clock reads, provisioning, device log, DFU. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { createSerialTransport, isSerialSupported } from "../transport/serial";
import { createFakeTransport } from "../transport/fake";
import { parseLine, parseEventLine, normalizeTelemetry, normalizePps, formatAck, isHexKey, contractOf, resetNames, NODE_MASTER } from "../lib/protocol";
import { createWirelessClock } from "../lib/wireless-clock";
import { WIRELESS_STATUS_MAX_AGE_MS, UNREADABLE_ALARM_REPEATS } from "../lib/constants";
import { MASTER_TICKS_PER_MS as TICKS_PER_MS } from "../lib/event-timing";
import { pipelineHealth as pipelineHealthOf } from "../lib/quality";
import * as eventLog from "../lib/eventLog";
import * as ppsLog from "../lib/ppsLog";
import * as deviceLog from "../lib/deviceLog";
import { useTimingStore } from "./timing";

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
  const lastError = ref(null);
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
  const logEntries = ref([]); // device log, newest first

  let transport = null;
  let wakeLock = null;
  let provisionPending = null; // { resolve, timer }
  let ackChain = Promise.resolve();
  let lastVerDrop = null;
  let identityLine = null;
  const repeats = new Map(); // unreadable line key -> count

  const clock = createWirelessClock({
    send: ({ request_id }) => {
      if (!transport?.connected) throw new Error("The master is not connected.");
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

  function pushConsole(dir, text) {
    const arr = consoleLines.value;
    arr.push({ t: Date.now(), dir, text });
    if (arr.length > CONSOLE_LIMIT) arr.splice(0, arr.length - CONSOLE_LIMIT);
  }

  function transmitLine(line) {
    if (!transport) return Promise.resolve(false);
    pushConsole("tx", line);
    return transport.write(line);
  }

  function drop(reason) {
    dropped.count++;
    dropped.reasons[reason] = (dropped.reasons[reason] || 0) + 1;
  }

  async function log(entry) {
    const row = await deviceLog.addEntry({ at: Date.now(), devid: identity.value?.devid ?? null, masterBootId: masterBootId.value, ...entry });
    logEntries.value = [row, ...logEntries.value].slice(0, LOG_VIEW_LIMIT);
    return row;
  }

  async function loadLog() {
    logEntries.value = (await deviceLog.listEntries()).slice(0, LOG_VIEW_LIMIT);
  }

  async function clearLog() {
    await deviceLog.clearEntries();
    logEntries.value = [];
  }

  async function exportLog() {
    return deviceLog.listEntries();
  }

  function raiseAlarm(key, text) {
    if (alarms.value.some((a) => a.key === key)) return;
    alarms.value = [...alarms.value, { key, text, at: Date.now() }];
    notyf.error(text);
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
    raiseAlarm("storage", `Browser storage failed (${durableError.value}) — new evidence and results are kept in memory only.`);
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
      case "I":
        onIdentity(msg, String(raw).trim());
        break;
      case "H":
        if (/^\d+$/.test(msg.nowTick || "")) heartbeat.value = { tick: BigInt(msg.nowTick), wallMs: now, uptimeMs: msg.uptimeMs, beaconSeq: msg.beaconSeq, nseen: msg.nseen };
        break;
      case "D":
        onTelemetry(msg.telemetry, now);
        break;
      case "E":
        await handleEventLine(msg.raw, now);
        break;
      case "T":
        clock.accept({ request_id: msg.requestId, master_tick: msg.masterTick, master_boot_id: msg.masterBootId });
        if (Number.isInteger(msg.masterBootId)) setMasterBoot(msg.masterBootId);
        break;
      case "P":
        onPps(msg.pps, now);
        break;
      case "A":
        if (msg.cmd === "K" && provisionPending) resolveProvision("ok");
        break;
      case "X":
        onErrorLine(msg, now);
        break;
      default:
        drop("unknown_line");
    }
  }

  function onIdentity(msg, raw) {
    const repeated = identityLine === raw; // the same boot answering ?ID again
    identityLine = raw;
    identity.value = msg;
    contract.value = contractOf(msg);
    const reason = Number.isInteger(msg.resetReason) ? resetNames(msg.resetReason).join(", ") : "unknown";
    if (!repeated) log({ kind: "boot", code: reason, text: raw, devid: msg.devid ?? null });
    if (!contract.value.ok) notyf.error(contract.value.reason);
    useTimingStore().onMasterVersion({ usbProto: msg.usbProto, radioProto: msg.radioProto });
  }

  function onTelemetry(raw, now) {
    const t = normalizeTelemetry(raw, now);
    if (!t) {
      drop("tel.format");
      return;
    }
    telemetry[t.node_id] = t;
    if (t.node_id !== NODE_MASTER) return;
    if (t.provisioned === 1) unprovisioned.value = false;
    if (t.master_boot_id != null) setMasterBoot(t.master_boot_id);
    if (t.ver_drop != null) {
      if (lastVerDrop != null && t.ver_drop > lastVerDrop) verDrop.value = { count: t.ver_drop, at: now };
      lastVerDrop = t.ver_drop;
    }
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

  function onPps(raw, now) {
    const p = normalizePps(raw, now);
    if (!p) {
      drop("pps");
      return;
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

  function onErrorLine(msg, now) {
    lastError.value = { reason: msg.reason, at: now };
    if (msg.reason === "noprov") {
      if (!unprovisioned.value) log({ kind: "X", code: msg.reason, text: msg.text });
      unprovisioned.value = true;
      return;
    }
    log({ kind: "X", code: msg.reason, text: msg.text });
    if (msg.reason === "keyfail" && provisionPending) resolveProvision("keyfail");
    else if (msg.reason === "clock") clock.close("The master rejected the clock request (X clock).");
  }

  function noteHead(hseq, now) {
    if (head.value?.hseq === hseq) head.value = { ...head.value, lastSeenAt: now };
    else head.value = { hseq, firstSeenAt: now, lastSeenAt: now };
  }

  function noteUnreadable(key, raw, reason) {
    const n = (repeats.get(key) || 0) + 1;
    repeats.set(key, n);
    drop(reason);
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
          notyf.error(`Quarantined an invalid event from ${p.node === NODE_MASTER ? "the master" : p.node ? `sensor ${p.node}` : "an unknown node"} (${p.reason}).`);
          useTimingStore().onEventRows([marker]);
          return;
        }
        const { duplicate, rows } = await eventLog.ingestLine({ rows: p.rows, masterBootId: p.masterBootId, hseq: p.hseq }, now);
        syncDurability();
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

  // Send the fleet key; resolves "ok" | "keyfail" | "timeout". Works for any board role.
  function provisionKey(hex) {
    if (!isHexKey(hex)) return Promise.reject(new Error("The key must be 64 hex characters."));
    if (!transport?.connected) return Promise.reject(new Error("The master is not connected."));
    if (provisionPending) return Promise.reject(new Error("A key transfer is already in progress."));
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolveProvision("timeout"), PROVISION_TIMEOUT_MS);
      provisionPending = { resolve, timer };
      pushConsole("tx", "K ****");
      transport.write(`K ${hex}`).then((sent) => {
        if (!sent) resolveProvision("timeout");
      });
    });
  }

  function readClock() {
    return clock.read();
  }

  // Ask the master to have every synced sensor checkpoint now (the next beacons carry the request).
  function requestCheckpoint() {
    if (!transport?.connected) return Promise.resolve(false);
    return transmitLine("CP");
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
    if (wasDfu) notyf.success("The board should now be in bootloader mode.");
    else notyf.error("The master was disconnected.");
  }

  function resetSession() {
    identity.value = null;
    contract.value = contractOf(null);
    heartbeat.value = null;
    pps.value = null;
    head.value = null;
    unprovisioned.value = false;
    lastVerDrop = null;
    identityLine = null;
    repeats.clear();
  }

  function finishDisconnect() {
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
      notyf.error("Web Serial is not supported by this browser. Use Chrome or Edge.");
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
    if (!transport) throw new Error("The master is not connected.");
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
    lastError,
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
    requestCheckpoint,
    gpsReport,
    useEdgesOf,
    provisionKey,
    enterBootloader,
    fakeTransport,
    handleLine,
    requestPersistence,
    loadLog,
    clearLog,
    exportLog,
    dismissAlarm,
    syncDurability,
  };
});
