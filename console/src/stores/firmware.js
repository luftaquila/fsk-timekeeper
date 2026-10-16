import { defineStore } from "pinia";
import { computed, ref, shallowRef } from "vue";
import { useDeviceStore } from "./device";
import { useHistoryStore } from "./history";
import { useTimingStore } from "./timing";
import { readFirmwarePackage, compareVersions } from "../firmware/package";
import { flashFirmware } from "../firmware/protocol";
import { USB_VID, USB_PID } from "../lib/constants";

const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const isApp = port => { const i = port.getInfo(); return i.usbVendorId === USB_VID && i.usbProductId === USB_PID; };
const isBoot = port => port.getInfo().usbVendorId === 0x239a;
const activePhases = new Set(["preparing", "bootloader", "erasing", "writing", "restarting", "verifying"]);

export const useFirmwareStore = defineStore("firmware", () => {
  const device = useDeviceStore();
  const timing = useTimingStore();
  const firmware = shallowRef(null);
  const filename = ref("");
  const phase = ref("idle");
  const progress = ref(0);
  const message = ref("");
  const expectedId = ref(null);
  let readingFile = 0;
  const busy = computed(() => activePhases.has(phase.value));
  const awaitingPort = computed(() => ["select-device", "select-bootloader", "select-app"].includes(phase.value));
  const actionLabel = computed(() => {
    const comparison = compareVersions(firmware.value?.metadata.version, device.identity?.fw);
    return comparison === 0 ? "Reinstall" : comparison === -1 ? "Downgrade" : "Update";
  });
  const canStart = computed(() => !!firmware.value?.compatible && !busy.value && !awaitingPort.value &&
    !device.updateInProgress && !device.clearingData && !useHistoryStore().importing && !timing.armed && !timing.starting && !timing.classifying && !device.connecting && device.transportKind !== "fake" && device.supported);

  function release() { device.updateInProgress = false; }
  function error(e) {
    phase.value = "error";
    message.value = e?.message || String(e);
    release();
  }

  async function selectFile(file) {
    if (busy.value || awaitingPort.value) return;
    const generation = ++readingFile;
    firmware.value = null; filename.value = file?.name || ""; message.value = ""; progress.value = 0;
    if (!file) { phase.value = "idle"; return; }
    phase.value = "checking";
    try {
      if (file.size > 32 * 1024 * 1024) throw new Error("ZIP file is too large (maximum 32 MB).");
      const parsed = await readFirmwarePackage(new Uint8Array(await file.arrayBuffer()));
      if (generation !== readingFile) return;
      firmware.value = parsed;
      phase.value = "ready";
      if (!parsed.compatible) message.value = "This firmware needs a console with matching USB and radio protocols. Open the console from the same release.";
    } catch (e) { if (generation === readingFile) error(e); }
  }

  async function start() {
    if (!canStart.value || device.clearingData) return;
    device.updateInProgress = true;
    expectedId.value = device.identity?.devid || null;
    message.value = ""; progress.value = 0;
    phase.value = "preparing";
    try {
      if (!device.connected) {
        phase.value = "select-device";
        message.value = "Select the connected board. Normal and bootloader ports are supported.";
        await chooseDevice();
        return;
      }
      await prepareBoard();
    } catch (e) { error(e); }
  }

  async function prepareBoard() {
    const appPort = await device.enterBootloader();
    phase.value = "bootloader";
    // Wait for the reset, then open the bootloader chooser. Never select a
    // remembered port silently; a browser may require a new click here.
    let ports = [];
    for (let i = 0; i < 50; i++) {
      ports = await navigator.serial.getPorts();
      if (!ports.includes(appPort)) break;
      await wait(100);
    }
    if (ports.includes(appPort)) throw new Error("The board did not enter bootloader mode. Double-tap RST, then retry the update.");
    phase.value = "select-bootloader";
    message.value = "Board reset requested. Select its bootloader port to continue. If it is not listed, double-tap RST on the board.";
    await chooseBootloader({ automatic: true });
  }

  async function chooseDevice() {
    if (phase.value !== "select-device") return;
    phase.value = "preparing";
    try {
      // Called directly by the button so the browser retains user activation.
      const port = await navigator.serial.requestPort({ filters: [
        { usbVendorId: USB_VID, usbProductId: USB_PID }, { usbVendorId: 0x239a },
      ] });
      if (isBoot(port)) { await transfer(port); return; }
      if (!isApp(port)) throw new Error("Select an FSK Timekeeper board or its bootloader.");
      if (!await device.connect({ port, forUpdate: true })) throw new Error("Could not connect to the board. Close other serial connections and retry.");
      for (let i = 0; i < 50 && !device.identity; i++) await wait(100);
      if (!device.identity) throw new Error("The board did not identify itself. Reconnect USB and retry.");
      expectedId.value = device.identity.devid;
      await prepareBoard();
    } catch (e) {
      if (e.name === "NotFoundError") { phase.value = "select-device"; return; }
      error(e);
    }
  }

  async function chooseBootloader({ automatic = false } = {}) {
    if (phase.value !== "select-bootloader") return;
    phase.value = "bootloader";
    try {
      const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x239a }] });
      if (!isBoot(port)) throw new Error("Select an Adafruit nRF52 bootloader port.");
      await transfer(port);
    } catch (e) {
      if (e.name === "NotFoundError") { phase.value = "select-bootloader"; return; }
      if (automatic && e.name === "SecurityError") {
        phase.value = "select-bootloader";
        message.value = "Board reset requested. Click Select Bootloader to open the device list.";
        return;
      }
      error(e);
    }
  }

  async function transfer(port) {
    if (timing.armed) throw new Error("Stop the running measurement first.");
    message.value = "Keep the board connected until the update finishes.";
    await flashFirmware(port, firmware.value, { onProgress: (state, value) => { phase.value = state; progress.value = value; } });
    phase.value = "select-app";
    message.value = "Firmware sent. Select the updated board to verify its version.";
  }

  async function chooseApp() {
    if (phase.value !== "select-app") return;
    const selection = navigator.serial.requestPort({ filters: [{ usbVendorId: USB_VID, usbProductId: USB_PID }] });
    phase.value = "verifying";
    try { await verify(await selection); }
    catch (e) {
      if (e.name === "NotFoundError") { phase.value = "select-app"; return; }
      error(e);
    }
  }

  async function verify(port) {
    if (!isApp(port)) throw new Error("Select the updated FSK Timekeeper board.");
    if (!await device.connect({ port, forUpdate: true })) {
      phase.value = "select-app";
      message.value = "Firmware sent, but reconnection failed. Select the updated board to retry verification.";
      return;
    }
    for (let i = 0; i < 50 && !device.identity; i++) await wait(100);
    const identity = device.identity;
    if (!identity) {
      await device.disconnect();
      phase.value = "select-app";
      message.value = "Firmware sent, but the board did not identify itself. Reconnect to verify.";
      return;
    }
    const m = firmware.value.metadata;
    if ((expectedId.value && identity.devid !== expectedId.value) || identity.product !== m.product ||
        identity.fw !== m.version || identity.usbProto !== m.usb_protocol || identity.radioProto !== m.radio_protocol) {
      await device.disconnect();
      throw new Error("Update could not be verified: board identity, version or protocols do not match the selected firmware.");
    }
    phase.value = "complete";
    message.value = `Firmware ${m.version} verified on board ${identity.devid}.`;
    release();
  }

  function cancel() {
    if (!awaitingPort.value) return;
    const sent = phase.value === "select-app";
    phase.value = sent ? "unverified" : "ready";
    message.value = sent ? "Firmware was sent; the running version has not been verified." : "Update cancelled before writing firmware.";
    release();
  }

  return { firmware, filename, phase, progress, message, expectedId, busy, awaitingPort, actionLabel, canStart,
    selectFile, start, chooseDevice, chooseBootloader, chooseApp, cancel };
});
