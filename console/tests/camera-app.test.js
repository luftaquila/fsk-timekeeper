// @vitest-environment jsdom
// The camera on the fake master: Camera Settings, Photo modes listed once a camera is set up, a
// Photo Laps heat of the cars that cross with an unread crossing the operator assigns, Stop, and
// the heat's cars in History.
import "fake-indexeddb/auto";
import { describe, it, beforeAll, afterAll, vi } from "vitest";
import assert from "node:assert/strict";
import { mount, flushPromises } from "@vue/test-utils";
import { createPinia } from "pinia";
import { createRouter, createMemoryHistory } from "vue-router";

const errors = [];
const origError = console.error;
let App, routes, useDeviceStore, useSettingsStore, useTimingStore, eventLog, fakeDefaults;

beforeAll(async () => {
  vi.spyOn(window, "confirm").mockReturnValue(true);
  window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
  console.error = (...a) => {
    errors.push(a.map(String).join(" "));
    origError(...a);
  };
  App = (await import("../src/App.vue")).default;
  routes = (await import("../src/router/index.js")).default.getRoutes().map((r) => ({ path: r.path, name: r.name, component: r.components?.default, redirect: r.redirect }));
  ({ useDeviceStore } = await import("../src/stores/device.js"));
  ({ useSettingsStore } = await import("../src/stores/settings.js"));
  ({ useTimingStore } = await import("../src/stores/timing.js"));
  eventLog = await import("../src/lib/eventLog.js");
  ({ fakeDefaults } = await import("../src/transport/fake.js"));
  fakeDefaults.slotDelayMin = 10;
  fakeDefaults.slotDelayMax = 30;
});

afterAll(() => {
  vi.restoreAllMocks();
  console.error = origError;
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, timeoutMs = 8000) {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("waitFor timeout");
    await sleep(25);
  }
}

describe("camera pages", () => {
  it("runs a camera-laps heat with an operator assignment", { timeout: 30000 }, async () => {
    localStorage.clear();
    eventLog._reset();
    const pinia = createPinia();
    const router = createRouter({ history: createMemoryHistory(), routes });
    const wrapper = mount(App, { global: { plugins: [pinia, router] }, attachTo: document.body });
    await router.isReady();
    await flushPromises();
    await waitFor(() => !wrapper.text().includes("Loading…"));
    const device = useDeviceStore(pinia);
    const settings = useSettingsStore(pinia);
    const timing = useTimingStore(pinia);

    await router.push("/settings");
    await flushPromises();
    assert.match(wrapper.text(), /Camera Settings/);
    assert.match(wrapper.text(), /Camera Pi/);
    assert.ok(wrapper.find('input[aria-label="Camera Host"]').exists());
    assert.doesNotMatch(wrapper.text(), /Cars/);

    // Photo modes are listed once a camera is set up; Start waits for it to be live.
    await router.push("/");
    await flushPromises();
    const options = () => wrapper.findAll("select.mode option").map((o) => o.element.value);
    assert.deepEqual(options(), ["sprint", "laps"]);
    settings.setCameraHost("cam.tail-1234.ts.net");
    await flushPromises();
    assert.deepEqual(options(), ["sprint", "laps", "camera-sprint", "camera-laps"]);
    assert.equal(await device.connect({ kind: "fake" }), true);
    const fake = device.fakeTransport();
    await flushPromises();
    assert.ok(timing.qualityFor("camera-laps").reasons.some((r) => /camera/.test(r.reason)));

    // Photo Sprint: the camera times the finish, the sensor mapped to Start the start.
    assert.deepEqual(timing.mappingsFor("camera-sprint"), [{ node_id: fake.cameraNode, role: "finish", label: "Camera" }]);
    assert.deepEqual(timing.missingRoles("camera-sprint"), ["start"]);

    fake.startCamera();
    await waitFor(() => device.cameraNode === fake.cameraNode && device.telemetry[fake.cameraNode]?.link_state === "online");
    await flushPromises();
    await wrapper.find("select.mode").setValue("camera-laps");
    await flushPromises();
    assert.equal(settings.state.mode, "camera-laps");
    assert.match(wrapper.text(), /Photo Laps/);
    await waitFor(() => timing.qualityFor("camera-laps").ok);
    // START needs a camera checkpoint, as it needs one from every sensor.
    await waitFor(() => eventLog.since(0).some((r) => r.node_id === fake.cameraNode && r.kind === "checkpoint"));
    const startBtn = wrapper.findAll("button").find((b) => b.text() === "Start");
    await startBtn.trigger("click");
    await waitFor(() => timing.run?.armed === true);
    assert.equal(timing.run.mode, "camera-laps");
    assert.deepEqual(timing.run.results, {});

    fake.cameraCrossing({ car: 1, conf: 99, offsetMs: 10 });
    fake.cameraCrossing({ car: 23, conf: 95, offsetMs: 20 });
    fake.cameraCrossing({ car: 1, conf: 98, offsetMs: 400 });
    const unread = fake.cameraCrossing({ car: 0, conf: 0, offsetMs: 900 });
    await waitFor(() => timing.live.crossings.length === 4);
    await flushPromises();
    assert.equal(timing.carRows.find((r) => r.car === "1").laps.length, 1);
    assert.match(wrapper.text(), /Unassigned/);
    const pending = timing.live.crossings.find((c) => c.tick === unread);
    assert.equal(pending.state, "unassigned");
    assert.equal(timing.run.results["23"].verification, "pending");

    // The operator assigns the unread crossing to 23: its lap appears.
    assert.deepEqual(timing.carRows.map((r) => r.car), ["1", "23"]);
    assert.equal(await timing.decide(pending.key, { car: "23" }), true);
    await flushPromises();
    assert.equal(timing.carRows.find((r) => r.car === "23").laps.length, 1);
    assert.equal(timing.run.results["23"].verification, "verified");
    assert.equal(timing.run.cameraCrossings.length, 4);

    await wrapper.findAll("button").find((b) => b.text() === "Stop").trigger("click");
    await waitFor(() => timing.run?.closed === true);
    for (const car of ["1", "23"]) assert.ok(timing.run.results[car].calibration, `car ${car} frozen`);

    // History lists the heat's cars with the other records, each with its photos.
    await router.push("/history");
    await flushPromises();
    const rows = wrapper.findAll("table.data-table tbody tr");
    assert.equal(rows.length, 2);
    assert.match(rows.map((r) => r.text()).join("\n"), /#1[\s\S]*#23|#23[\s\S]*#1/);
    assert.ok(rows.every((r) => r.text().includes("Photo Laps")));
    await rows[0].findAll("button").find((b) => b.text() === "Photos").trigger("click");
    await flushPromises();
    const header = wrapper.find(".detail-header").text();
    assert.match(header, /Photo Laps/);
    assert.match(header, /Car (1|23)\b/);

    await device.disconnect();
    wrapper.unmount();
    assert.deepEqual(errors, []);
  });
});
