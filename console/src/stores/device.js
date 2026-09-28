/* Master link: transport, line dispatch, telemetry, clock reads, provisioning, DFU. */
import { defineStore } from "pinia";
import { ref, reactive, computed } from "vue";
import { useNotification } from "../composables/useNotification";
import { createSerialTransport, isSerialSupported } from "../transport/serial";
import { createFakeTransport } from "../transport/fake";
import { parseLine, validateEvent, normalizeTelemetry, normalizePps, formatAck, isHexKey, NODE_MASTER } from "../lib/protocol";
import { createWirelessClock } from "../lib/wireless-clock";
import { WIRELESS_STATUS_MAX_AGE_MS, USB_PRODUCT } from "../lib/constants";
import { MASTER_TICKS_PER_MS as TICKS_PER_MS } from "../lib/event-timing";
import * as eventLog from "../lib/eventLog";
import { useTimingStore } from "./timing";

const CONSOLE_LIMIT = 300;
const PROVISION_TIMEOUT_MS = 3000;

export const useDeviceStore = defineStore("device", () => {
  const notyf = useNotification();

  const connected = ref(false);
  const connecting = ref(false);
  const transportKind = ref(null); // "serial" | "fake"
  const identity = ref(null); // { product, fw, devid, freqMhz, sf, bw, ticksPerMs }
  const identityOk = ref(false);
  const lastLineAt = ref(0);
  const heartbeat = ref(null); // { tick: bigint, wallMs, uptimeMs, beaconSeq, nseen }
  const telemetry = reactive({}); // node_id -> normalized telemetry
  const masterBootId = ref(null);
  const pps = ref(null); // latest GPS/PPS report (P line), see protocol.normalizePps
  const unprovisioned = ref(false);
  const lastError = ref(null);
  const dropped = reactive({ count: 0, reasons: {} });
  const stats = reactive({ lines: 0, events: 0, duplicates: 0, acks: 0 });
  const consoleLines = ref([]);
  const dfuInProgress = ref(false);
  const durable = ref(true);
  const durableError = ref(null);

  let transport = null;
  let wakeLock = null;
  let provisionPending = null; // { resolve, timer }
  let ackChain = Promise.resolve();

  const clock = createWirelessClock({
    send: ({ request_id }) => {
      if (!transport?.connected) throw new Error("The master is not connected.");
      transmitLine(`T ${request_id}`);
    },
  });

  const supported = computed(() => isSerialSupported());

  function masterFresh(now = Date.now()) {
    return connected.value && now - lastLineAt.value <= WIRELESS_STATUS_MAX_AGE_MS;
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

  async function handleLine(raw) {
    const now = Date.now();
    lastLineAt.value = now;
    stats.lines++;
    const msg = parseLine(raw);
    if (!msg) return;
    if ((msg.type !== "H" && msg.type !== "P") || consoleLines.value.length < CONSOLE_LIMIT) pushConsole("rx", raw.trim());
    const timing = useTimingStore();
    switch (msg.type) {
      case "I":
        identity.value = msg;
        identityOk.value = msg.product === USB_PRODUCT;
        if (!identityOk.value) notyf.error(`Unexpected device identity: ${msg.product}`);
        break;
      case "H":
        if (/^\d+$/.test(msg.nowTick || "")) heartbeat.value = { tick: BigInt(msg.nowTick), wallMs: now, uptimeMs: msg.uptimeMs, beaconSeq: msg.beaconSeq, nseen: msg.nseen };
        break;
      case "D": {
        const t = normalizeTelemetry(msg.telemetry, now);
        if (!t) {
          drop("tel.node_id");
          break;
        }
        telemetry[t.node_id] = t;
        if (t.node_id === NODE_MASTER) {
          if (t.provisioned === 1) unprovisioned.value = false;
          if (t.master_boot_id != null) {
            if (masterBootId.value != null && masterBootId.value !== t.master_boot_id) pps.value = null;
            masterBootId.value = t.master_boot_id;
            timing.onMasterBoot(t.master_boot_id);
          }
        }
        timing.onTelemetry(t.node_id);
        break;
      }
      case "E":
        await handleEvent(msg.event, now);
        break;
      case "T":
        clock.accept({ request_id: msg.requestId, master_tick: msg.masterTick, master_boot_id: msg.masterBootId });
        if (Number.isInteger(msg.masterBootId)) masterBootId.value = msg.masterBootId;
        break;
      case "P": {
        const p = normalizePps(msg.pps, now);
        if (p) pps.value = p;
        else drop("pps");
        break;
      }
      case "A":
        if (msg.cmd === "K" && provisionPending) resolveProvision("ok");
        break;
      case "X":
        lastError.value = { reason: msg.reason, at: now };
        if (msg.reason === "noprov") unprovisioned.value = true;
        else if (msg.reason === "keyfail" && provisionPending) resolveProvision("keyfail");
        else if (msg.reason === "clock") clock.close("The master rejected the clock request (X clock).");
        break;
      default:
        drop("unknown_line");
    }
  }

  async function handleEvent(event, now) {
    const check = validateEvent(event);
    if (!check.ok) {
      drop(check.reason);
      return;
    }
    stats.events++;
    // Serialize: seq order, IDB commit, then the ack, then the engine.
    ackChain = ackChain
      .then(async () => {
        const { row, duplicate } = await eventLog.ingest(check.row, now);
        if (eventLog.isDurable() !== durable.value) {
          durable.value = eventLog.isDurable();
          durableError.value = eventLog.getInitError()?.message || null;
        }
        // Only now may the master evict it from its RAM delivery queue.
        if (await transmitLine(formatAck(row))) stats.acks++;
        if (duplicate) {
          stats.duplicates++;
          return;
        }
        useTimingStore().onEventRows([row]);
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

  // Send the fleet key; resolves "ok" | "keyfail" | "timeout".
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

  // Calibration to freeze into a run: only a valid, fresh PPS estimate counts. null = nominal.
  function ppsCalibration(now = Date.now()) {
    const p = pps.value;
    if (!p || p.valid !== 1 || now - p.at > 3000) return null;
    return { ppb: p.ppb, ppsTick: p.tick, utc: p.utc, fix: p.fix, sats: p.sats, span: p.span };
  }

  /* Screen Wake Lock: keep the bridge tab awake while connected. */
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

  function finishDisconnect() {
    connected.value = false;
    connecting.value = false;
    dfuInProgress.value = false;
    identityOk.value = false;
    heartbeat.value = null;
    pps.value = null;
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
      identity.value = null;
      identityOk.value = false;
      unprovisioned.value = false;
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
    lastLineAt,
    heartbeat,
    telemetry,
    masterBootId,
    pps,
    unprovisioned,
    lastError,
    dropped,
    stats,
    consoleLines,
    dfuInProgress,
    durable,
    durableError,
    masterFresh,
    tickToWallMs,
    connect,
    disconnect,
    transmitLine,
    readClock,
    ppsCalibration,
    provisionKey,
    enterBootloader,
    fakeTransport,
    handleLine,
  };
});
