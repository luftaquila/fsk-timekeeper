// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { mount } from "@vue/test-utils";
import { nextTick, reactive } from "vue";
import NodeDiagnostics from "../src/components/NodeDiagnostics.vue";
import NodeAge from "../src/components/NodeAge.vue";

let device, diagnostics, age;
vi.mock("../src/stores/device", () => ({ useDeviceStore: () => device }));
beforeEach(() => {
  vi.useFakeTimers();
  device = reactive({ connected: true, telemetry: {} });
});
afterEach(() => { diagnostics?.unmount(); age?.unmount(); vi.useRealTimers(); });
function report(state, heardAt) {
  device.telemetry.A = {
    node_id: "A", link_state: state, received_at: Date.now(), last_seen_at: heardAt,
    provisioned: 1, clock_source: "xtal", sync_valid: 1, skew_valid: 1,
  };
}
function render() {
  diagnostics = mount(NodeDiagnostics, { props: { nodeId: "A" } });
  age = mount(NodeAge, { props: { nodeId: "A" } });
}
const link = () => diagnostics.findAll(".diagnostic-grid > div")[0];

describe("sensor link and report freshness", () => {
  it("stays ONLINE → STALE → LOST with periodic reports and preserves the last heard time", async () => {
    const heardAt = Date.now();
    report("online", heardAt); render();
    for (let second = 1; second <= 60; second++) {
      await vi.advanceTimersByTimeAsync(1000);
      const state = second <= 12 ? "online" : second <= 17 ? "degraded" : "lost";
      if (second % 5 === 0 || second === 13 || second === 18) report(state, heardAt);
      await nextTick();
      expect(diagnostics.find(".no-report").exists()).toBe(false);
      expect(link().get("dt").text()).toBe("Link");
      expect(link().get("dd").text()).toBe(second <= 12 ? "ONLINE" : second <= 17 ? "STALE" : "LOST");
      expect(age.classes().includes("badge-success")).toBe(second <= 12);
      expect(device.telemetry.A.last_seen_at).toBe(heardAt);
    }
    expect(age.attributes("aria-label")).toBe("Last Heard: 1m");
  });

  it("shows NO REPORT in the existing Link field when reports stop, then recovers", async () => {
    report("lost", Date.now() - 20000); render();
    await vi.advanceTimersByTimeAsync(13000);
    expect(diagnostics.find(".no-report").exists()).toBe(false);
    expect(link().get("dt").text()).toBe("Link");
    expect(link().get("dd").text()).toBe("NO REPORT");
    expect(link().get("dd").attributes("title")).toBe("Last reported link: LOST");
    report("online", Date.now()); await nextTick();
    expect(diagnostics.find(".no-report").exists()).toBe(false);
    expect(link().get("dd").text()).toBe("ONLINE");
    expect(age.classes()).toContain("badge-success");
  });

  it("marks a fresh ONLINE report as historical immediately on USB disconnect", async () => {
    report("online", Date.now()); render();
    device.connected = false; await nextTick();
    expect(diagnostics.find(".no-report").exists()).toBe(false);
    expect(link().get("dt").text()).toBe("Link");
    expect(link().get("dd").text()).toBe("NO REPORT");
    expect(link().get("dd").attributes("title")).toBe("Last reported link: ONLINE");
    expect(link().get("dd").classes()).toContain("warn");
    expect(age.classes()).toContain("badge-warning");
  });
});
