// @vitest-environment jsdom
// The camera session on the camera Pi: until a camera-mode Start holds it the camera is judged by
// whether it can start; a Start holds it and arms on its first checkpoint; Stop lets it go, as
// does a Start that fails. The Pi's bridge is the fake master, its HTTP API a stand-in.
import "fake-indexeddb/auto";
import { describe, it, vi } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";

const toasts = { success: [], error: [], warning: [] };
vi.mock("../src/composables/useNotification", () => ({
  useNotification: () => ({
    success: (m) => toasts.success.push(m),
    error: (m) => toasts.error.push(m),
    open: ({ message }) => toasts.warning.push(message),
  }),
}));

const bridge = { fake: null };
vi.mock("../src/transport/pi.js", async () => {
  const { createFakeTransport } = await import("../src/transport/fake.js");
  return { PI_BUSY_CLOSE: 4409, createPiTransport: (callbacks) => (bridge.fake = createFakeTransport(callbacks)) };
});

// The Pi: its camera online, and a new hold starting a camera session on the master, answered
// once the session reports (a hold said again keeps the session, as pipeline.hold does).
const pi = { camera: true, running: false, held: false, holds: [], fail: null };
vi.mock("../src/lib/cameraApi.js", async (importOriginal) => {
  const actual = await importOriginal();
  const { USB_PROTO_VERSION } = await import("../src/lib/protocol.js");
  return {
    ...actual,
    cameraApi: () => ({
      info: async () => ({ api: actual.CAMERA_API, app: "test", usbProto: USB_PROTO_VERSION,
        camera: { model: "JHEL0201GM", connected: pi.camera }, session: { running: pi.running }, storage: {} }),
      config: async () => ({ values: {}, limits: {} }),
      setSession: async (run) => {
        pi.holds.push(run);
        const fresh = run && !pi.held;
        pi.held = run;
        if (!run) {
          pi.running = false;
          return { run, session: null };
        }
        if (pi.fail) throw new Error(pi.fail);
        const { useDeviceStore } = await import("../src/stores/device.js");
        const device = useDeviceStore();
        const node = bridge.fake.cameraNode;
        const before = device.telemetry[node]?.sensor_boot_id ?? null;
        if (!fresh && pi.running) return { run, session: before };
        bridge.fake.startCamera();
        await waitFor(() => (device.telemetry[node]?.sensor_boot_id ?? null) !== before);
        pi.running = true;
        return { run, session: device.telemetry[node].sensor_boot_id };
      },
    }),
  };
});

globalThis.requestAnimationFrame = (cb) => setTimeout(() => cb(Date.now()), 16);
globalThis.cancelAnimationFrame = (id) => clearTimeout(id);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, timeoutMs = 5000) {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("waitFor timeout");
    await sleep(20);
  }
}

describe("camera session on the camera Pi", () => {
  it("is held from a camera-mode Start until Stop", { timeout: 30000 }, async () => {
    const { fakeDefaults } = await import("../src/transport/fake.js");
    fakeDefaults.slotDelayMin = 10;
    fakeDefaults.slotDelayMax = 30;
    const eventLog = await import("../src/lib/eventLog.js");
    const { useDeviceStore } = await import("../src/stores/device.js");
    const { useSettingsStore } = await import("../src/stores/settings.js");
    const { useTimingStore } = await import("../src/stores/timing.js");
    const { useCameraStore } = await import("../src/stores/camera.js");
    localStorage.clear();
    setActivePinia(createPinia());
    eventLog._reset();
    await eventLog.init();
    const timing = useTimingStore();
    await timing.reevaluate();
    const device = useDeviceStore();
    const settings = useSettingsStore();
    const camera = useCameraStore();

    settings.setCameraHost("cam.tail-1234.ts.net");
    assert.equal(await camera.connect(), true);
    await waitFor(() => pi.holds.includes(false)); // a console that connects holds nothing
    await waitFor(() => timing.qualityFor("camera-laps").ok);
    assert.equal(device.telemetry[bridge.fake.cameraNode], undefined); // no camera session yet

    pi.camera = false;
    await camera.refresh();
    assert.deepEqual(timing.qualityFor("camera-laps").reasons.map((r) => r.reason), ["Camera offline"]);
    pi.camera = true;
    await camera.refresh();

    pi.holds.length = 0;
    assert.equal(await timing.start("camera-laps"), true, toasts.error.join(" | "));
    const node = bridge.fake.cameraNode;
    assert.equal(pi.holds[0], true);
    assert.equal(timing.run.nodes[node].boot, device.telemetry[node].sensor_boot_id);

    await timing.stop();
    await waitFor(() => pi.holds.at(-1) === false);

    pi.fail = "Camera session did not start: camoff";
    pi.holds.length = 0;
    assert.equal(await timing.start("camera-laps"), false);
    assert.match(toasts.error.at(-1), /Camera session did not start: camoff/);
    await waitFor(() => pi.holds.at(-1) === false); // the failed Start lets it go
    assert.equal(timing.run.closed, true);
    await device.disconnect();
    assert.equal(device.telemetry[node], undefined); // the camera node does not stay behind as a sensor
  });
});
