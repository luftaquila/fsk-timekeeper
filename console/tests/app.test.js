// @vitest-environment jsdom
// Mounts the real app (router, stores, every view) under jsdom and drives it with the
// fake master: catches template/runtime errors that the pure-lib tests cannot.
import "fake-indexeddb/auto";
import { describe, it, beforeAll, afterAll } from "vitest";
import assert from "node:assert/strict";
import { mount, flushPromises } from "@vue/test-utils";
import { createPinia } from "pinia";
import { createRouter, createMemoryHistory } from "vue-router";

const errors = [];
const origError = console.error;
const origWarn = console.warn;

let App, routes, useDeviceStore, useSettingsStore, useTimingStore, eventLog;

beforeAll(async () => {
  // jsdom gaps
  window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
  console.error = (...a) => {
    errors.push(a.map(String).join(" "));
    origError(...a);
  };
  console.warn = (...a) => {
    errors.push(a.map(String).join(" "));
  };
  App = (await import("../src/App.vue")).default;
  const routerModule = await import("../src/router/index.js");
  routes = routerModule.default.getRoutes().map((r) => ({ path: r.path, name: r.name, component: r.components?.default, props: r.props?.default, redirect: r.redirect }));
  ({ useDeviceStore } = await import("../src/stores/device.js"));
  ({ useSettingsStore } = await import("../src/stores/settings.js"));
  ({ useTimingStore } = await import("../src/stores/timing.js"));
  eventLog = await import("../src/lib/eventLog.js");
});

afterAll(() => {
  console.error = origError;
  console.warn = origWarn;
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(pred, timeoutMs = 5000) {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > timeoutMs) throw new Error("waitFor timeout");
    await sleep(25);
  }
}

describe("app under jsdom", () => {
  it("renders every route, runs a sprint and shows the laps page", { timeout: 30000 }, async () => {
    localStorage.clear();
    eventLog._reset();
    const pinia = createPinia();
    const router = createRouter({ history: createMemoryHistory(), routes });
    const wrapper = mount(App, { global: { plugins: [pinia, router] }, attachTo: document.body });
    await router.isReady();
    await flushPromises();
    await waitFor(() => wrapper.text().includes("Start → Finish"));
    assert.match(wrapper.text(), /FSK Timekeeper Console/);
    assert.match(wrapper.text(), /Master not connected/);
    assert.ok(wrapper.find(".link-dot.bad").exists());

    const device = useDeviceStore(pinia);
    const settings = useSettingsStore(pinia);
    const timing = useTimingStore(pinia);

    for (const [path, needle] of [
      ["/settings", /Sensor mapping/],
      ["/history", /No runs recorded yet/],
      ["/", /Start → Finish/],
    ]) {
      await router.push(path);
      await flushPromises();
      assert.match(wrapper.text(), needle, path);
    }

    // connect the simulator and check the settings tables fill in
    await router.push("/settings");
    await flushPromises();
    assert.equal(await device.connect({ kind: "fake" }), true);
    const fake = device.fakeTransport();
    await waitFor(() => Object.keys(device.telemetry).length >= 3);
    await flushPromises();
    assert.match(wrapper.text(), /Connected/);
    assert.match(wrapper.text(), /FSK-WL 1\.0\.0/);
    assert.ok(wrapper.find(".link-dot.ok").exists());
    const [a, b] = [...fake.sensors.keys()];
    assert.ok(wrapper.text().includes(a) && wrapper.text().includes(b), "diagnostics/mapping list the fake sensors");
    settings.setMapping(a, { role: "start" });
    settings.setMapping(b, { role: "finish" });
    await waitFor(() => fake.queueLength === 0 && device.stats.acks >= 2);

    // run a sprint from the timing page's own buttons
    await router.push("/");
    await flushPromises();
    await waitFor(() => wrapper.text().includes("Ready."));
    await wrapper.find("input.note").setValue("car 7");
    await flushPromises();
    assert.equal(wrapper.find(".run-note").text(), "car 7", "note shows in the timer card while typing");
    const startBtn = wrapper.findAll("button").find((btn) => btn.text() === "Start");
    assert.ok(startBtn, "Start button");
    await startBtn.trigger("click");
    await waitFor(() => timing.run?.armed === true);
    await flushPromises();
    assert.ok(wrapper.find(".traffic-light.green").exists(), "green light");
    assert.ok(wrapper.find("select.mode").attributes("disabled") != null, "mode select locked while armed");
    assert.equal(timing.run.note, "car 7");
    assert.equal(wrapper.find(".run-note").text(), "car 7", "note shows in the timer card during the run");

    fake.crossing(a);
    fake.crossing(b, { offsetMs: 800 });
    await waitFor(() => timing.live.crossings.length === 2);
    await flushPromises();
    assert.match(wrapper.text(), /Start sensor/);
    assert.match(wrapper.text(), /\+00:00\.8\d\d/);
    await sleep(900);
    fake.checkpoint(a);
    fake.checkpoint(b);
    await waitFor(() => timing.run.verification === "verified");
    await flushPromises();
    assert.match(wrapper.text(), /official/);
    assert.match(wrapper.text(), /GPS-calibrated [+−]\d+\.\d\d ppm/);
    assert.ok(wrapper.find(".traffic-light.red").exists(), "red light after completion");

    await router.push("/history");
    await flushPromises();
    await waitFor(() => wrapper.text().includes("verified"));
    assert.match(wrapper.text(), /Start → Finish/);

    // switch to laps: the closed run is cleared and the laps layout renders
    await router.push("/");
    await flushPromises();
    await wrapper.find("select.mode").setValue("laps");
    await flushPromises();
    assert.equal(timing.run, null);
    assert.match(wrapper.text(), /Lap times/);
    assert.ok(wrapper.find("input.lap-target").exists(), "auto-stop input");

    await device.disconnect();
    await flushPromises();
    wrapper.unmount();

    const real = errors.filter((e) => !/Not implemented: HTMLCanvasElement|navigation \(except hash changes\)/.test(e));
    assert.deepEqual(real, [], "no console errors/warnings during the session");
  });
});
