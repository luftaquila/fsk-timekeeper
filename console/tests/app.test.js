// @vitest-environment jsdom
// Mounts the whole app in jsdom on the fake master to catch template/runtime errors lib tests miss.
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
    assert.match(device.identity.devid, /^[0-9A-F]{16}$/);
    assert.equal(wrapper.find(".device-id").text(), "00000001");
    assert.match(wrapper.text(), /v1\.0\.0/);
    assert.ok(wrapper.find(".link-dot.ok").exists());

    // GPS qualification and PPS measurement share the Clock Drift field.
    const previousPps = device.pps;
    const ppsReport = { tick: "16000000", valid: 0, fix: 0, ppb: 0, span: 0, at: Date.now() };
    const masterStatus = wrapper.find(".master-diagnostics");
    assert.ok(!masterStatus.findAll("dt").some((label) => label.text() === "GPS"));
    for (const [pps, expected] of [
      [null, "NO PPS"],
      [{ ...ppsReport, tick: "0" }, "NO PPS"],
      [ppsReport, "MEASURING (NO FIX)"],
      [{ ...ppsReport, fix: 1 }, "MEASURING"],
      [{ ...ppsReport, fix: 1, span: 8 }, "NOT READY"],
      [{ ...ppsReport, ppb: 1230, span: 8 }, "+1.23 ppm (NO FIX)"],
      [{ ...ppsReport, ppb: -1230, span: 16 }, "−1.23 ppm (NO FIX)"],
      [{ ...ppsReport, span: 8 }, "+0.00 ppm (NO FIX)"],
      [{ ...ppsReport, valid: 1, ppb: 1230, span: 8 }, "+1.23 ppm (NO FIX)"],
      [{ ...ppsReport, valid: 1, fix: 1, ppb: 1230, span: 8 }, "+1.23 ppm"],
      [{ ...ppsReport, valid: 1, fix: 1, ppb: -1230, span: 16 }, "−1.23 ppm"],
      [{ ...ppsReport, valid: 1, fix: 1, span: 8 }, "+0.00 ppm"],
      [{ ...ppsReport, valid: 1, at: Date.now() - 60000 }, "NO PPS"],
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
      ["cfgapply", /Saved, Radio Start Failed/],
      ["cfgpending", /Saved, Apply Pending/],
      ["cfgfail", /Save Failed/],
    ]) {
      const spy = vi.spyOn(fake, "write").mockImplementation(async (line) => {
        if (!line.startsWith("R ")) return write(line);
        if (code !== "cfgfail") {
          await device.handleLine(`I FSK-WL 8 ${device.identity.fw} ${device.identity.devid} M 17 923100 7 250.00 16000 0 -3 0`);
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
      assert.equal(sensor.find(".remove-sensor").exists(), false);
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
    await waitFor(() => timing.qualityFor("sprint").ok);
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
    assert.match(wrapper.findAll(".sensors .card")[1].find(".crossing-time").text(), /^00:00\.8\d\d$/);
    // Each role has one sensor: the two first crossings decide the sprint on arrival.
    await waitFor(() => timing.run.verification === "verified");
    await waitFor(() => timing.run.durationNs != null, 4000);
    await flushPromises();
    assert.match(wrapper.find(".result-time").text(), /^00:00\.8\d\d$/);
    assert.ok(wrapper.find(".traffic-light.green").exists(), "keeps running after the first finish");
    assert.equal(timing.armed, true);
    assert.equal(timing.run.closed, false);
    const firstResult = timing.run.durationNs;
    const { useHistoryStore } = await import("../src/stores/history");
    const history = useHistoryStore(pinia);
    await waitFor(() => history.rows.length === 1 && history.rows[0].durationNs === firstResult);
    assert.equal(history.rows[0].closed, false, "first finish saves before Stop");
    const elapsed = timing.live.clockDisplay;
    await waitFor(() => timing.live.clockDisplay !== elapsed);
    await sleep(400); // past the 300 ms debounce window of both first crossings
    fake.crossing(a);
    fake.crossing(b, { offsetMs: 800 });
    await waitFor(() => timing.live.crossings.length === 4);
    await flushPromises();
    assert.equal(wrapper.findAll(".sensors .card")[0].findAll(".rec-list li").length, 2);
    assert.equal(wrapper.findAll(".sensors .card")[1].findAll(".rec-list li").length, 2);
    assert.equal(timing.run.durationNs, firstResult);
    assert.equal(history.rows.length, 1, "later crossings do not add another result");
    assert.equal(history.rows[0].durationNs, firstResult);
    // Stop must freeze the elapsed clock, not replace it with the first finish result.
    const stopNow = Date.now();
    timing.live.startedAt = stopNow - 12345;
    const nowSpy = vi.spyOn(Date, "now").mockReturnValue(stopNow);
    await wrapper.findAll("button").find(btn => btn.text() === "Stop").trigger("click");
    nowSpy.mockRestore();
    await flushPromises();
    assert.equal(timing.armed, false);
    assert.ok(wrapper.find(".traffic-light.red").exists(), "stops only after Stop");
    const stoppedClock = timing.live.clockDisplay;
    assert.equal(stoppedClock, "00:12.345");
    assert.equal(timing.run.durationNs, firstResult);
    fake.crossing(a);
    await sleep(100);
    assert.equal(timing.live.crossings.length, 4);
    assert.equal(timing.live.clockDisplay, stoppedClock);

    await router.push("/history");
    await flushPromises();
    await waitFor(() => wrapper.find(".status-select").element?.selectedOptions[0]?.textContent === "OK");
    assert.match(wrapper.text(), /Start → Finish/);

    // A saved fault must remain readable without hovering over a status chip.
    const runLog = await import("../src/lib/runLog.js");
    const saved = history.rows[0];
    await runLog.save({ ...saved, fault: { reasons: [{ role: "finish", reason: `Sensor ${b}: Capture Loss`, lo: saved.boundaryTick, hi: null }] } });
    await flushPromises();
    assert.match(wrapper.find(".fault-row").text(), /Finish/);
    assert.match(wrapper.find(".fault-row").text(), /Capture Loss/);
    assert.match(wrapper.find(".fault-row .fault-window").text(), /end not known yet/);
    await runLog.save(saved);

    // Start/Stop without a crossing must not classify the attempt as DNS.
    await router.push("/");
    await flushPromises();
    await wrapper.findAll("button").find((btn) => btn.text() === "Start").trigger("click");
    await waitFor(() => timing.run?.armed === true);
    await wrapper.findAll("button").find((btn) => btn.text() === "Stop").trigger("click");
    await flushPromises();
    assert.match(wrapper.text(), /Stopped/);
    assert.doesNotMatch(wrapper.find(".result-body").text(), /DNS|DNF/);
    assert.ok(wrapper.find(".traffic-light.red").exists());
    await router.push("/history");
    await flushPromises();
    assert.equal(wrapper.find(".status-select").element.value, "verified");
    assert.equal(history.rows.length, 1, "only the completed sprint is recorded");

    // switch to laps: the closed run is cleared and the laps layout renders
    await router.push("/");
    await flushPromises();
    await wrapper.find("select.mode").setValue("laps");
    await flushPromises();
    assert.equal(timing.run, null);
    assert.match(wrapper.text(), /Lap Times/);
    assert.ok(wrapper.find("input.lap-target").exists(), "auto-stop input");
    settings.setLapTarget(null);
    assert.equal(await timing.start("laps"), true);
    const t0 = BigInt(fake.crossing(a));
    for (const offset of [2000n, 3500n, 6000n, 8500n]) {
      fake.crossing(a, { at: t0 + offset * 16000n });
    }
    await waitFor(() => timing.lapRows.length === 4);
    await flushPromises();
    const lapRows = wrapper.findAll(".lap-table tbody tr");
    assert.deepEqual(lapRows.map((row) => row.findAll("td")[2].text()), ["0.000", "+1.000", "−0.500", "-"]);
    assert.deepEqual(lapRows.map((row) => row.findAll("td")[2].classes().includes("slower")), [false, true, false, false]);
    assert.equal(lapRows[2].findAll("td")[3].text(), "BEST");
    await timing.stop();
    await waitFor(() => history.rows.some((r) => r.mode === "laps" && r.crossingTicks.length === 5));
    await router.push("/history");
    await flushPromises();
    const resultLink = wrapper.find(".result-link");
    assert.equal(resultLink.text(), "00:08.500");
    // jsdom does not implement the native dialog top layer.
    const dialog = wrapper.find("dialog").element;
    dialog.showModal = () => dialog.setAttribute("open", "");
    dialog.close = () => {
      dialog.removeAttribute("open");
      dialog.dispatchEvent(new Event("close"));
    };
    await resultLink.trigger("click");
    await flushPromises();
    assert.ok(dialog.hasAttribute("open"));
    assert.deepEqual(wrapper.findAll(".lap-stats dd").map((el) => el.text()), ["4", "00:01.500 (#2)", "00:02.125", "00:08.500"]);
    const popupRows = wrapper.findAll(".lap-list tbody tr");
    assert.deepEqual(popupRows.map((row) => row.findAll("td")[2].classes().includes("slower")), [false, false, true, false]);
    const displayExport = history.exportData();
    assert.deepEqual(displayExport.History.find((r) => r.Mode === "Laps")["Lap Times"].map((r) => [String(r.Lap), r.Time, r["Δ Before"], r["Δ Best"]]),
      popupRows.map((row) => row.findAll("td").map((td) => td.text())));
    assert.deepEqual(displayExport.History.map(({ Date, Mode, Note, Result, Status, Calibration }) => ({ Date, Mode, Note, Result, Status, Calibration })), wrapper.findAll(".data-table tbody > tr:not(.fault-row)").map((row) => {
      const cells = row.findAll("td");
      const select = row.find(".status-select").element;
      const status = [select.selectedOptions[0].textContent,
        ...(cells[4].findAll("span.badge").map((badge) => badge.text()))].join("\n");
      return { Date: cells[0].text(), Mode: cells[1].text(), Note: cells[2].text(), Result: cells[3].text(), Status: status, Calibration: cells[5].text() };
    }));
    assert.equal(wrapper.findAll(".lap-list .best").length, 1);
    await wrapper.find('[aria-label="Close lap details"]').trigger("click");
    await flushPromises();
    assert.ok(!dialog.hasAttribute("open"));
    assert.equal(wrapper.find(".lap-list").exists(), false);

    // Classification is editable on a measured row and preserves its times.
    await wrapper.find(".status-select").setValue("DSQ");
    await waitFor(() => timing.run.classification === "DSQ" && !timing.classifying);
    await flushPromises();
    assert.equal(wrapper.find(".status-select").element.value, "DSQ");
    assert.equal(wrapper.find('.status-select option[value=""]').text(), "Clear");
    assert.ok(wrapper.findAll("button").some((btn) => btn.text() === "Export Excel"));
    assert.equal(wrapper.find('input[type="checkbox"]').exists(), false);
    assert.equal(wrapper.find(".result-link").text(), "00:08.500");
    await wrapper.find(".status-select").setValue("");
    await waitFor(() => timing.run.classification === null && !timing.classifying);
    await flushPromises();
    assert.equal(wrapper.find(".status-select").element.value, "verified");
    assert.equal(wrapper.find(".result-link").text(), "00:08.500");

    await device.disconnect();
    await router.push("/");
    await flushPromises();
    await wrapper.findAll("button").find((btn) => btn.text() === "Clear").trigger("click");
    await wrapper.findAll("button").find((btn) => btn.text() === "DNS").trigger("click");
    await waitFor(() => timing.run?.classification === "DNS" && !timing.classifying);
    await flushPromises();
    assert.equal(history.status(history.rows[0]), "DNS");
    assert.match(wrapper.find(".result-body").text(), /DNS/);
    const count = history.rows.length;
    const classificationButton = (label) => wrapper.findAll("button").find((btn) => btn.text() === label);
    for (const value of ["DNS", "DNF", "DSQ"]) {
      if (value !== "DNS") {
        await classificationButton(value).trigger("click");
        await waitFor(() => timing.run.classification === value && !timing.classifying);
        await flushPromises();
      }
      assert.equal(classificationButton(value).attributes("aria-pressed"), "true");
      await classificationButton(value).trigger("click");
      await waitFor(() => timing.run.classification === null && !timing.classifying);
      await flushPromises();
      assert.equal(classificationButton(value).attributes("aria-pressed"), "false");
      assert.equal(history.rows.length, count - 1);
      assert.doesNotMatch(wrapper.find(".result-body").text(), /DNS|DNF|DSQ/);
    }
    await router.push("/history");
    await flushPromises();
    const clearHistory = wrapper.findAll("button").find((btn) => btn.text() === "Clear All");
    const recordCount = history.rows.length;
    assert.ok(recordCount > 0);
    vi.mocked(window.confirm).mockReturnValueOnce(false);
    await clearHistory.trigger("click");
    await flushPromises();
    assert.equal(history.rows.length, recordCount);
    assert.equal(clearHistory.text(), "Clear All");
    vi.mocked(window.confirm).mockReturnValueOnce(true);
    await clearHistory.trigger("click");
    await flushPromises();
    await waitFor(() => history.rows.length === 0 && !history.clearing);
    assert.equal(device.logEntries.length, 0);
    await flushPromises();
    assert.equal(clearHistory.element.disabled, false);
    await device.handleLine("X badcmd");

    await router.push("/settings");
    await flushPromises();
    await wrapper.find(".log-header").trigger("click");
    await flushPromises();
    const clearLog = wrapper.findAll("button").find((btn) => btn.text() === "Clear All");
    const logCount = device.logEntries.length;
    assert.ok(logCount > 0);
    vi.mocked(window.confirm).mockReturnValueOnce(false);
    await clearLog.trigger("click");
    await flushPromises();
    assert.equal(device.logEntries.length, logCount);
    assert.equal(clearLog.text(), "Clear All");
    vi.mocked(window.confirm).mockReturnValueOnce(true);
    await clearLog.trigger("click");
    await flushPromises();
    await waitFor(() => device.logEntries.length === 0);
    wrapper.unmount();

    const real = errors.filter((e) => !/Not implemented: HTMLCanvasElement|navigation \(except hash changes\)/.test(e));
    assert.deepEqual(real, [], "no console errors/warnings during the session");
  });
});
