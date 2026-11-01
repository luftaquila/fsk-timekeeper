import { beforeEach, afterEach, describe, expect, it, vi } from "vitest";
import { createPinia, setActivePinia } from "pinia";
import { reactive } from "vue";
import { useFirmwareStore } from "../src/stores/firmware";
import { firmwareFixture } from "./firmware-fixture";

const { flash } = vi.hoisted(() => ({ flash: vi.fn() }));
let device, timing;
vi.mock("../src/stores/device", () => ({ useDeviceStore: () => device }));
vi.mock("../src/stores/timing", () => ({ useTimingStore: () => timing }));
vi.mock("../src/firmware/protocol", async importOriginal => ({ ...await importOriginal(), flashFirmware: flash }));
const app = { getInfo: () => ({ usbVendorId: 0x1999, usbProductId: 0x0515 }) };
const boot = { getInfo: () => ({ usbVendorId: 0x239a, usbProductId: 0x0029 }) };
const identity = { product: "FSK-WL", devid: "0123456789ABCDEF", fw: "v1.2.3", usbProto: 8, radioProto: 17 };
let serial;
async function ready(options) {
  const store = useFirmwareStore();
  const zip = firmwareFixture(options).zip();
  await store.selectFile({ name: "release.zip", size: zip.length, arrayBuffer: async () => zip });
  return store;
}
beforeEach(() => {
  setActivePinia(createPinia());
  timing = reactive({ armed: false, starting: false, classifying: false });
  device = reactive({ connected: false, connecting: false, identity: null, supported: true, updateInProgress: false, transportKind: null,
    enterBootloader: vi.fn(async () => { device.connected = false; device.identity = null; return app; }),
    connect: vi.fn(async () => { device.connected = true; device.identity = { ...identity }; return true; }),
    disconnect: vi.fn(async () => { device.connected = false; device.identity = null; }),
  });
  serial = { getPorts: vi.fn(async () => []), requestPort: vi.fn(async () => boot) };
  vi.stubGlobal("navigator", { serial });
  flash.mockReset(); flash.mockImplementation(async (port, firmware, { onProgress }) => onProgress("restarting", 100));
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

describe("firmware upgrade lifecycle", () => {
  it("opens the normal/bootloader chooser directly from Upgrade, then chooses the reset board", async () => {
    serial.requestPort.mockResolvedValueOnce(app).mockResolvedValue(boot);
    serial.getPorts.mockResolvedValueOnce([app]).mockResolvedValue([boot]);
    const store = await ready();
    const updating = store.start();
    expect(serial.requestPort).toHaveBeenCalledWith({ filters: [
      { usbVendorId: 0x1999, usbProductId: 0x0515 }, { usbVendorId: 0x239a },
    ] });
    await updating;
    expect(device.enterBootloader).toHaveBeenCalledOnce();
    expect(store.expectedId).toBe(identity.devid);
    expect(serial.requestPort).toHaveBeenNthCalledWith(2, { filters: [{ usbVendorId: 0x239a }] });
    expect(flash).toHaveBeenCalledOnce();
    expect(store.phase).toBe("select-app");
    expect(device.connect).toHaveBeenCalledTimes(1);
    serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("complete"); expect(device.updateInProgress).toBe(false);
  });
  it("reports a failed mode switch instead of opening an empty bootloader chooser", async () => {
    device.connected = true; device.identity = { ...identity };
    serial.getPorts.mockResolvedValue([app]);
    const store = await ready(); vi.useFakeTimers();
    const updating = store.start(); await vi.advanceTimersByTimeAsync(5100); await updating;
    expect(store.phase).toBe("error");
    expect(store.message).toContain("did not enter bootloader mode");
    expect(serial.requestPort).not.toHaveBeenCalled(); expect(flash).not.toHaveBeenCalled();
    expect(device.updateInProgress).toBe(false);
  });
  it("offers a bootloader button only when a fresh gesture is required", async () => {
    serial.requestPort.mockResolvedValueOnce(app).mockImplementationOnce(() => {
      throw new DOMException("User gesture expired", "SecurityError");
    }).mockResolvedValue(boot);
    const store = await ready(); await store.start();
    expect(store.phase).toBe("select-bootloader"); expect(store.message).toContain("Click Select Bootloader");
    expect(flash).not.toHaveBeenCalled(); expect(device.updateInProgress).toBe(true);
    await store.chooseBootloader();
    expect(store.phase).toBe("select-app"); expect(flash).toHaveBeenCalledOnce();
  });
  it("supports a board already in bootloader without another selection step", async () => {
    const store = await ready(); await store.start();
    expect(serial.requestPort).toHaveBeenCalledOnce(); expect(flash).toHaveBeenCalledOnce();
    expect(device.enterBootloader).not.toHaveBeenCalled(); expect(device.connect).not.toHaveBeenCalled();
    expect(store.phase).toBe("select-app"); expect(store.message).toContain("Firmware sent");
    serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("complete"); expect(device.updateInProgress).toBe(false);
  });
  it("opens a bootloader chooser for a connected board and never reconnects for verification automatically", async () => {
    device.connected = true; device.identity = { ...identity, fw: "v0.1.0", usbProto: 1 };
    serial.getPorts.mockResolvedValue([boot]);
    const store = await ready(); await store.start();
    expect(device.enterBootloader).toHaveBeenCalledOnce();
    expect(serial.requestPort).toHaveBeenCalledExactlyOnceWith({ filters: [{ usbVendorId: 0x239a }] });
    expect(store.phase).toBe("select-app"); expect(device.connect).not.toHaveBeenCalled();
    expect(serial.getPorts).toHaveBeenCalledOnce();
    serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("complete"); expect(device.connect).toHaveBeenCalledWith({ port: app, forUpdate: true });
  });
  it("does not silently select an authorized bootloader and lets the user retry a cancelled chooser", async () => {
    device.connected = true; device.identity = { ...identity }; serial.getPorts.mockResolvedValue([boot]);
    serial.requestPort.mockRejectedValueOnce(new DOMException("cancel", "NotFoundError")).mockResolvedValue(boot);
    const store = await ready(); await store.start();
    expect(store.phase).toBe("select-bootloader"); expect(flash).not.toHaveBeenCalled();
    await store.chooseBootloader(); expect(flash).toHaveBeenCalledOnce();
  });
  it("keeps initial chooser cancellation recoverable and prevents double requests", async () => {
    serial.requestPort.mockRejectedValue(new DOMException("cancel", "NotFoundError"));
    const store = await ready();
    const updating = store.start(); await store.start(); await updating;
    expect(serial.requestPort).toHaveBeenCalledOnce(); expect(store.phase).toBe("select-device");
    await store.chooseDevice(); expect(serial.requestPort).toHaveBeenCalledTimes(2);
    store.cancel(); expect(store.phase).toBe("ready"); expect(device.updateInProgress).toBe(false);
  });
  it("blocks measurement", async () => {
    const store = await ready();
    for (const key of ["armed", "starting", "classifying"]) {
      timing[key] = true; await store.start(); expect(store.canStart).toBe(false); timing[key] = false;
    }
    expect(serial.requestPort).not.toHaveBeenCalled(); expect(flash).not.toHaveBeenCalled();
  });
  it("flashes and verifies an incompatible package after warning", async () => {
    device.connected = true; device.identity = { ...identity };
    serial.getPorts.mockResolvedValue([boot]);
    const store = await ready({ usb_protocol: 99 });
    expect(store.firmware.compatible).toBe(false); expect(store.message).toMatch(/protocol differs/); expect(store.canStart).toBe(true);
    await store.start(); expect(flash).toHaveBeenCalledOnce(); expect(store.phase).toBe("select-app");
    device.connect.mockImplementation(async () => { device.connected = true; device.identity = { ...identity, usbProto: 99 }; return true; });
    serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("complete");
  });
  it("labels reinstallation and downgrade explicitly", async () => {
    device.identity = { ...identity }; const store = await ready(); expect(store.actionLabel).toBe("Reinstall");
    device.identity.fw = "v1.10.0"; expect(store.actionLabel).toBe("Downgrade");
  });
  it("rejects wrong-board verification even if versions match", async () => {
    device.connected = true; device.identity = { ...identity, devid: "0000000000000001" };
    serial.getPorts.mockResolvedValue([boot]);
    const store = await ready(); await store.start();
    expect(store.phase).toBe("select-app");
    serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("error"); expect(device.disconnect).toHaveBeenCalled(); expect(device.updateInProgress).toBe(false);
  });
  it("never reports completion when transfer fails or version differs", async () => {
    const store = await ready(); flash.mockRejectedValueOnce(new Error("ACK timeout"));
    await store.start(); expect(store.phase).toBe("error"); expect(device.updateInProgress).toBe(false);
    await store.start();
    device.connect.mockImplementation(async () => { device.identity = { ...identity, fw: "v0.0.0" }; return true; });
    serial.requestPort.mockResolvedValue(app); await store.chooseApp(); expect(store.phase).toBe("error");
  });
  it("distinguishes successful transmission from failed reconnection", async () => {
    const store = await ready(); await store.start();
    device.connect.mockResolvedValue(false); serial.requestPort.mockResolvedValue(app); await store.chooseApp();
    expect(store.phase).toBe("select-app"); store.cancel(); expect(store.phase).toBe("unverified"); expect(device.updateInProgress).toBe(false);
  });
});
