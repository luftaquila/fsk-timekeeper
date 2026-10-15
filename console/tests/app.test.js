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
  vi.spyOn(window, "confirm").mockReturnValue(true);
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
  vi.restoreAllMocks();
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
      ["/settings", /Debounce/],
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
    for (const selector of [".master-header h3 .dot", ".link-dot"]) {
      assert.ok(wrapper.find(selector).classes().includes("bad"));
    }
    device.connecting = true;
    await flushPromises();
    for (const selector of [".master-header h3 .dot", ".link-dot"]) {
      assert.ok(wrapper.find(selector).classes().includes("connecting"));
      assert.equal(wrapper.find(selector).attributes("aria-label"), "Connecting");
    }
    device.connecting = false;
    assert.equal(await device.connect({ kind: "fake" }), true);
    let fake = device.fakeTransport();
    await waitFor(() => Object.keys(device.telemetry).length >= 3);
    await flushPromises();
    assert.equal(wrapper.find(".master-header h3 .dot").attributes("aria-label"), "Connected");
    assert.match(wrapper.text(), /v1\.0\.0/);
    assert.ok(wrapper.find(".link-dot.ok").exists());

    // GPS qualification and PPS measurement share the Clock Drift field.
    const previousPps = device.pps;
    const ppsReport = { valid: 0, fix: 0, ppb: 0, span: 0, at: Date.now() };
    const masterStatus = wrapper.find(".master-diagnostics");
    assert.ok(!masterStatus.findAll("dt").some((label) => label.text() === "GPS"));
    for (const [pps, expected] of [
      [null, "NO REPORT"],
      [ppsReport, "NO GPS"],
      [{ ...ppsReport, fix: 1 }, "NOT READY"],
      [{ ...ppsReport, valid: 1, ppb: 1230, span: 8 }, "+1.23 ppm"],
      [{ ...ppsReport, valid: 1, ppb: -1230, span: 16 }, "−1.23 ppm"],
      [{ ...ppsReport, valid: 1, span: 8 }, "+0.00 ppm"],
      [{ ...ppsReport, valid: 1, at: Date.now() - 60000 }, "NO REPORT"],
    ]) {
      device.pps = pps;
      await flushPromises();
      const field = masterStatus.findAll("dt").find((label) => label.text() === "Clock Drift").element.parentElement;
      assert.equal(field.querySelector("dd").textContent.trim().replace(/\s+/g, " "), expected);
    }
    device.pps = previousPps;

    // radio settings: the board answers R with its new I line (not a new boot), then A R OK
    assert.equal(wrapper.find("details.board-settings").element.open, false);
    wrapper.find("details.board-settings").element.open = true;
    assert.equal(wrapper.find(".frequency-current").text(), "922.900 MHz");
    assert.equal(wrapper.find(".power-current").text(), "12 dBm");
    assert.equal(wrapper.find(".lbt-current").text(), "On");
    assert.equal(wrapper.find("input.freq").element.value, "922.900");
    assert.equal(wrapper.find(".key-input").attributes("maxlength"), "64");
    assert.doesNotMatch(wrapper.text(), /Load from board|More diagnostics/);
    assert.equal(wrapper.findAll(".node-diagnostics details").length, 0);
    assert.ok(wrapper.findAll("h3").some((heading) => heading.text().includes("Sensors")));

    // Repeated reports preserve edits; a fresh connection loads its own saved values.
    await wrapper.find("input.freq").setValue("900.001");
    await fake.write("?ID");
    await flushPromises();
    assert.equal(wrapper.find("input.freq").element.value, "900.001");
    await device.disconnect();
    await flushPromises();
    assert.equal(wrapper.find("input.freq").element.value, "");
    assert.equal(await device.connect({ kind: "fake" }), true);
    fake = device.fakeTransport();
    await waitFor(() => device.identity && Object.keys(device.telemetry).length >= 3);
    await flushPromises();
    assert.equal(wrapper.find("input.freq").element.value, "922.900");
    const boots = device.logEntries.filter((e) => e.kind === "boot").length;
    assert.ok(boots >= 1);
    assert.equal(await device.configureRadio({ freqKhz: 923100, powerDbm: -3, lbt: 0 }), "ok");
    await flushPromises();
    assert.equal(wrapper.find(".frequency-current").text(), "923.100 MHz");
    assert.equal(wrapper.find(".power-current").text(), "-3 dBm");
    assert.equal(wrapper.find(".lbt-current").text(), "Off");
    assert.equal(device.logEntries.filter((e) => e.kind === "boot").length, boots);
    const applyRadio = wrapper.find('[aria-labelledby="radio-title"] button.send');
    for (const invalid of ["", "802.999", "930.001", "923.0001", "9e2", "abc"]) {
      await wrapper.find("input.freq").setValue(invalid);
      assert.equal(applyRadio.element.disabled, true, invalid);
    }
    await wrapper.find("select.lbt").setValue("0");
    for (const [mhz, dbm, expected] of [
      ["930", "22", "930.000 MHz"],
      ["803", "-9", "803.000 MHz"],
      ["923.101", "-3", "923.101 MHz"],
    ]) {
      await wrapper.find("input.freq").setValue(mhz);
      await wrapper.find("select.power").setValue(dbm);
      await applyRadio.trigger("click");
      await waitFor(() => wrapper.find(".frequency-current").text() === expected && applyRadio.text() === "Apply");
      await flushPromises();
      assert.equal(wrapper.find(".frequency-current").text(), expected);
      assert.equal(wrapper.find(".power-current").text(), `${dbm} dBm`);
      assert.equal(wrapper.find(".lbt-current").text(), "Off");
      assert.equal(wrapper.find(".config-status").exists(), false);
      assert.equal(applyRadio.element.disabled, true);
    }
    // Default restores firmware values only after confirmation, using the normal apply path.
    const defaultRadio = wrapper.find('[aria-labelledby="radio-title"] .restore-defaults');
    const beforeDefault = wrapper.find("input.freq").element.value;
    vi.mocked(window.confirm).mockReturnValueOnce(false);
    await defaultRadio.trigger("click");
    assert.equal(wrapper.find("input.freq").element.value, beforeDefault);
    assert.equal(wrapper.find(".frequency-current").text(), "923.101 MHz");
    await defaultRadio.trigger("click");
    await waitFor(() => wrapper.find(".frequency-current").text() === "922.900 MHz" && applyRadio.text() === "Apply");
    await flushPromises();
    assert.equal(wrapper.find(".power-current").text(), "12 dBm");
    assert.equal(wrapper.find(".lbt-current").text(), "On");
    assert.equal(wrapper.find('[aria-labelledby="radio-title"] .section-actions .action-result').text(), "Saved");
    assert.equal(defaultRadio.element.disabled, true);
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
          await device.handleLine(`I FSK-WL 5 ${device.identity.fw} ${device.identity.devid} M 14 923100 7 250.00 16000 0 -3 0`);
        }
        await device.handleLine(`X ${code}`);
        return true;
      });
      try {
        assert.equal(await device.configureRadio({ freqKhz: 923100, powerDbm: -3, lbt: 0 }), code);
        await wrapper.find("input.freq").setValue("923.200");
        await wrapper.find("select.power").setValue("-3");
        const apply = wrapper.find('[aria-labelledby="radio-title"] button.send');
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
    for (const [node, role] of [[a, "start"], [b, "finish"]]) {
      const sensor = wrapper.find(`article[aria-label="Sensor ${node}"]`);
      assert.match(sensor.text(), /Timing/);
      assert.match(sensor.text(), /Battery/);
      await sensor.find("select").setValue(role);
      assert.equal(settings.mappingOf(node).role, role);
      assert.equal(sensor.find("button").exists(), false);
      const note = sensor.find(".sensor-note input");
      assert.equal(note.attributes("placeholder"), "Note");
      await note.setValue(`Gate ${node}`);
      assert.equal(settings.mappingOf(node).note, `Gate ${node}`);
      await sensor.find("select").setValue("unused");
      assert.equal(settings.mappingOf(node).role, "unused");
      assert.equal(settings.mappingOf(node).note, `Gate ${node}`);
      await sensor.find("select").setValue(role);
      const stored = JSON.parse(localStorage.getItem("tk.settings.v2")).mapping[node];
      assert.equal(stored.role, role);
      assert.equal(stored.note, `Gate ${node}`);
    }
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
    assert.match(wrapper.text(), /Start Sensor/);
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
    assert.match(wrapper.text(), /Lap Times/);
    assert.ok(wrapper.find("input.lap-target").exists(), "auto-stop input");

    await device.disconnect();
    await flushPromises();
    wrapper.unmount();

    const real = errors.filter((e) => !/Not implemented: HTMLCanvasElement|navigation \(except hash changes\)/.test(e));
    assert.deepEqual(real, [], "no console errors/warnings during the session");
  });
});
