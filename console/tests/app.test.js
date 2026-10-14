// @vitest-environment jsdom
// Mounts the real app (router, stores, every view) under jsdom and drives it with the
// fake master: catches template/runtime errors that the pure-lib tests cannot.
import "fake-indexeddb/auto";
import { describe, it, beforeAll, afterAll, vi } from "vitest";
import assert from "node:assert/strict";
import { mount, flushPromises } from "@vue/test-utils";
import { createPinia } from "pinia";
import { createRouter, createMemoryHistory } from "vue-router";

const errors = [];
const origError = console.error;
const origWarn = console.warn;

let App, routes, useDeviceStore, useSettingsStore, useTimingStore, eventLog, fakeDefaults;

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
  ({ fakeDefaults } = await import("../src/transport/fake.js"));
  fakeDefaults.slotDelayMin = 10;
  fakeDefaults.slotDelayMax = 30;
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

    // radio settings: the board answers R with its new I line (not a new boot), then A R OK
    assert.match(wrapper.text(), /922\.900 MHz · 12 dBm · LBT on/);
    const boots = device.logEntries.filter((e) => e.kind === "boot").length;
    assert.ok(boots >= 1);
    assert.equal(await device.configureRadio({ freqKhz: 923100, powerDbm: -3, lbt: 0 }), "ok");
    await flushPromises();
    assert.match(wrapper.text(), /923\.100 MHz · -3 dBm · LBT off/);
    assert.equal(device.logEntries.filter((e) => e.kind === "boot").length, boots);
    // Apply errors still report the stored target, but must never look like success.
    const write = fake.write;
    for (const [code, message] of [
      ["cfgapply", /Settings stored, but radio startup failed/],
      ["cfgpending", /Application is pending until board initialization succeeds/],
      ["cfgfail", /Settings could not be stored/],
    ]) {
      const spy = vi.spyOn(fake, "write").mockImplementation(async (line) => {
        if (!line.startsWith("R ")) return write(line);
        if (code !== "cfgfail") {
          await device.handleLine(`I FSK-WL 4 1.0.0 ${device.identity.devid} M 13 923100 7 250.00 16000 0 -3 0`);
        }
        await device.handleLine(`X ${code}`);
        return true;
      });
      try {
        assert.equal(await device.configureRadio({ freqKhz: 923100, powerDbm: -3, lbt: 0 }), code);
        await wrapper.find("input.freq").setValue("923.100");
        await wrapper.find("input.power").setValue("-3");
        const apply = wrapper.findAll("button").find((btn) => btn.text() === "Apply to connected board");
        await apply.trigger("click");
        await flushPromises();
        assert.match(wrapper.text(), message);
        assert.doesNotMatch(wrapper.text(), /Radio settings stored and applied/);
        assert.equal(device.logEntries.filter((e) => e.kind === "boot").length, boots);
      } finally {
        spy.mockRestore();
      }
    }
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
    // Each role has one sensor: the two first crossings decide the sprint on arrival.
    await waitFor(() => timing.run.verification === "verified");
    await waitFor(() => timing.run.durationNs != null, 4000);
    await flushPromises();
    assert.match(wrapper.text(), /official/);
    assert.match(wrapper.text(), /GPS|nominal 16 MHz/);
    assert.match(wrapper.find(".result-time").text(), /^00:00\.8\d\d$/);
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
