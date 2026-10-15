// @vitest-environment jsdom
import { describe, it, vi } from "vitest";
import assert from "node:assert/strict";
import { createPinia } from "pinia";
import { mount } from "@vue/test-utils";
import { nextTick } from "vue";
import NodeDiagnostics from "../src/components/NodeDiagnostics.vue";
import { useDeviceStore } from "../src/stores/device";
import { ERROR_BITS, MASTER_QUEUE_CAPACITY } from "../src/lib/protocol";
import { pipelineHealth } from "../src/lib/quality";

vi.mock("../src/composables/useNotification", () => ({
  useNotification: () => ({ success() {}, error() {} }),
}));

describe("current master queue status", () => {
  it("recovers despite legacy latched flags and 751 historical retries, then warns on a new full queue", async () => {
    const pinia = createPinia();
    const device = useDeviceStore(pinia);
    device.telemetry = { 0: {
      node_id: "0", received_at: Date.now(), link_state: "online",
      provisioned: 1, clock_source: "xtal", queue_depth: 16, queue_overflow: 751,
      err_flags: ERROR_BITS.QUEUE_FULL | ERROR_BITS.USB_DROP,
    } };
    const wrapper = mount(NodeDiagnostics, { props: { nodeId: "0" }, global: { plugins: [pinia] } });
    const timing = () => wrapper.find(".timing-value");
    const errors = () => wrapper.find(".error-chips").text();
    try {
      assert.equal(timing().text(), "QUEUE FULL");
      assert.ok(timing().classes().includes("warn"));
      assert.match(errors(), /host queue full/);
      assert.equal(pipelineHealth({ master: device.telemetry[0] }).ok, false);

      device.telemetry[0].queue_depth = 8;
      await nextTick();
      assert.match(timing().text(), /^8\s+QUEUED$/);
      assert.doesNotMatch(errors(), /host queue full/);

      device.telemetry[0].queue_depth = 0;
      await nextTick();
      assert.equal(timing().text(), "OK");
      assert.ok(timing().classes().includes("good"));
      assert.doesNotMatch(errors(), /host queue full/);
      assert.match(errors(), /USB line dropped/); // unrelated errors survive
      assert.equal(device.telemetry[0].queue_overflow, 751);
      assert.equal(pipelineHealth({ master: device.telemetry[0] }).ok, true);

      device.telemetry[0].err_flags = ERROR_BITS.QUEUE_FULL;
      await nextTick();
      assert.equal(wrapper.find(".diagnostic-errors").exists(), false);

      // Full even before the first refused enqueue: no historical count is needed.
      device.telemetry[0].err_flags = 0;
      device.telemetry[0].queue_overflow = 0;
      device.telemetry[0].queue_depth = MASTER_QUEUE_CAPACITY;
      await nextTick();
      assert.equal(timing().text(), "QUEUE FULL");
      assert.match(errors(), /host queue full/);
      assert.equal(pipelineHealth({ master: device.telemetry[0] }).ok, false);
    } finally {
      wrapper.unmount();
    }
  });
});
