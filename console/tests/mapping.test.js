// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { mount } from "@vue/test-utils";
import { createPinia, setActivePinia } from "pinia";
import { nextTick, reactive } from "vue";
import MappingCard from "../src/components/MappingCard.vue";
import { useSettingsStore } from "../src/stores/settings";
import { WIRELESS_STATUS_MAX_AGE_MS } from "../src/lib/constants";

let device, timing, wrapper;
vi.mock("../src/stores/device", () => ({ useDeviceStore: () => device }));
vi.mock("../src/stores/timing", () => ({ useTimingStore: () => timing }));
beforeEach(() => {
  vi.useFakeTimers(); localStorage.clear(); setActivePinia(createPinia());
  device = reactive({ connected: true, admission: {}, telemetry: {}, updateInProgress: false });
  timing = reactive({ armed: false, starting: false, classifying: false });
});
afterEach(() => { wrapper?.unmount(); vi.useRealTimers(); });
function render() {
  wrapper = mount(MappingCard, { global: { stubs: { NodeAge: true, NodeDiagnostics: true } } });
  return wrapper;
}
function report(state) { return { link_state: state, received_at: Date.now() }; }

describe("missing sensor removal", () => {
  it("only offers Remove for LOST or absent reports and removes the entry and mapping", async () => {
    device.telemetry = { "0": report("lost"), "00000001": report("online"), "00000002": report("degraded"), "00000003": report("lost") };
    const settings = useSettingsStore();
    settings.setMapping("00000003", { role: "finish", note: "old gate" });
    settings.setMapping("00000004", { role: "start" });
    render();
    expect(wrapper.findAll(".sensor")).toHaveLength(4);
    expect(wrapper.findAll(".remove-sensor")).toHaveLength(2);
    await wrapper.get('[aria-label="Sensor 00000003"] .remove-sensor').trigger("click");
    expect(wrapper.find('[aria-label="Sensor 00000003"]').exists()).toBe(false);
    expect(settings.mappingOf("00000003")).toBeNull();
    device.telemetry["00000003"] = report("lost"); await nextTick();
    expect(wrapper.find('[aria-label="Sensor 00000003"]').exists()).toBe(false);
    expect(wrapper.get(".sensor-count").text()).toBe("3");
  });
  it("updates availability when reports expire and blocks removal during a run", async () => {
    device.telemetry["00000001"] = report("online"); render();
    expect(wrapper.find(".remove-sensor").exists()).toBe(false);
    await vi.advanceTimersByTimeAsync(WIRELESS_STATUS_MAX_AGE_MS + 1000);
    expect(wrapper.find(".remove-sensor").exists()).toBe(true);
    timing.armed = true; await nextTick();
    expect(wrapper.get(".remove-sensor").element.disabled).toBe(true);
    timing.armed = false; await nextTick();
    await wrapper.get(".remove-sensor").trigger("click");
    expect(wrapper.findAll(".sensor")).toHaveLength(0);
  });
});

describe("role mapping", () => {
  // Firefox rebuilds an open dropdown on any <select> subtree mutation, even a
  // same-value attribute write, and resets its highlight to the old option.
  it("leaves the role select untouched while the card re-renders", async () => {
    device.telemetry["00000001"] = report("online"); render();
    const select = wrapper.get('[aria-label="Sensor 00000001"] select');
    const records = [];
    const observer = new MutationObserver((r) => records.push(...r));
    observer.observe(select.element, { subtree: true, childList: true, attributes: true, characterData: true });
    await vi.advanceTimersByTimeAsync(1000);
    device.telemetry["00000001"] = report("degraded"); await nextTick();
    records.push(...observer.takeRecords());
    observer.disconnect();
    expect(records).toHaveLength(0);
    await select.setValue("start");
    expect(useSettingsStore().mappingOf("00000001").role).toBe("start");
    expect(select.element.value).toBe("start");
  });
});
