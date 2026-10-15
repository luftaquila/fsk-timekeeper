// @vitest-environment jsdom
import { beforeEach, afterEach, describe, it, vi } from "vitest";
import assert from "node:assert/strict";
import { reactive } from "vue";
import { mount, flushPromises } from "@vue/test-utils";
import DeviceLogCard from "../src/components/DeviceLogCard.vue";
import { downloadBlob } from "../src/lib/format";

const device = reactive({ logEntries: [], exportLog: vi.fn(), clearLog: vi.fn() });
vi.mock("../src/stores/device", () => ({ useDeviceStore: () => device }));
vi.mock("../src/composables/useNotification", () => ({ useNotification: () => ({ success: vi.fn() }) }));
vi.mock("../src/lib/format", async (original) => ({ ...await original(), downloadBlob: vi.fn() }));

let wrapper;
const rows = () => wrapper.findAll("tbody tr");
const button = (name) => wrapper.find(`[aria-label="${name} log page"]`);
const range = () => wrapper.find(".page-range").text();

beforeEach(async () => {
  device.logEntries = Array.from({ length: 80 }, (_, i) => ({
    id: 80 - i, at: Date.now() - i * 1000, kind: i < 55 ? "X" : i < 70 ? "boot" : i < 75 ? "console" : "radio",
    code: `entry_${80 - i}`, text: `detail ${80 - i}`, devid: "F4AEF4AE00000001",
  }));
  device.exportLog.mockImplementation(async () => [...device.logEntries]);
  device.clearLog.mockImplementation(async () => { device.logEntries = []; });
  wrapper = mount(DeviceLogCard);
  await wrapper.find(".log-header").trigger("click");
});
afterEach(() => { wrapper.unmount(); vi.restoreAllMocks(); vi.clearAllMocks(); });

describe("device log navigation", () => {
  it("bounds rendered rows and supports 10, 25 and 50 rows per page", async () => {
    assert.equal(rows().length, 10);
    assert.equal(range(), "1–10 of 80");
    await wrapper.find('[aria-label="Log rows per page"]').setValue("25");
    assert.ok(button("Previous").element.disabled);
    await button("Next").trigger("click");
    assert.equal(range(), "26–50 of 80");
    assert.equal(rows()[0].findAll("td")[2].text(), "entry_55");
    await button("Last").trigger("click");
    assert.equal(rows().length, 5);
    assert.ok(button("Next").element.disabled);
    await button("Previous").trigger("click");
    assert.equal(range(), "51–75 of 80");
    const size = wrapper.find('[aria-label="Log rows per page"]');
    assert.deepEqual(size.findAll("option").map((o) => o.text()), ["10", "25", "50"]);
    for (const n of [10, 25, 50]) {
      await size.setValue(String(n));
      assert.equal(rows().length, n);
      assert.equal(range(), `1–${n} of 80`);
      await button("Last").trigger("click");
    }
    await button("First").trigger("click");
    assert.equal(range(), "1–50 of 80");
  });

  it("filters before pagination and resets pages when the type changes", async () => {
    await wrapper.find('[aria-label="Log rows per page"]').setValue("25");
    await button("Last").trigger("click");
    const type = wrapper.find('[aria-label="Device log type"]');
    await type.setValue("X");
    assert.equal(range(), "1–25 of 55");
    assert.ok(rows().every((r) => r.findAll("td")[1].text() === "Error"));
    await button("Last").trigger("click");
    assert.equal(range(), "51–55 of 55");
    for (const [kind, label, count] of [["boot", "Boot", 15], ["console", "Console", 5], ["radio", "Radio", 5]]) {
      await type.setValue(kind);
      assert.equal(range(), `1–${count} of ${count}`);
      assert.ok(rows().every((r) => r.findAll("td")[1].text() === label));
    }
    device.logEntries = device.logEntries.filter((e) => e.kind !== "radio");
    await flushPromises();
    assert.equal(wrapper.find(".empty-state").text(), "No matching entries");
    assert.equal(range(), "0–0 of 0");
    assert.ok(button("Next").element.disabled);
    await type.setValue("");
    assert.equal(rows().length, 25);
  });

  it("keeps live updates bounded and clamps the page after entries disappear", async () => {
    await wrapper.find('[aria-label="Log rows per page"]').setValue("25");
    await button("Last").trigger("click");
    device.logEntries.unshift({ id: 81, kind: "console", code: "new", at: Date.now() });
    await flushPromises();
    assert.equal(range(), "76–81 of 81");
    device.logEntries = device.logEntries.slice(0, 30);
    await flushPromises();
    assert.equal(range(), "26–30 of 30");
    vi.spyOn(window, "confirm").mockReturnValue(true);
    await wrapper.findAll("button").find((b) => b.text() === "Clear All").trigger("click");
    await flushPromises();
    assert.equal(rows().length, 0);
    assert.equal(range(), "0–0 of 0");
    assert.equal(wrapper.find(".empty-state").text(), "No entries");
  });

  it("exports matching entries from every page", async () => {
    await wrapper.find('[aria-label="Device log type"]').setValue("X");
    await button("Last").trigger("click");
    await wrapper.findAll("button").find((b) => b.text() === "Export JSON").trigger("click");
    await flushPromises();
    const blob = vi.mocked(downloadBlob).mock.calls[0][0];
    const text = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.onerror = reject;
      reader.readAsText(blob);
    });
    const { entries } = JSON.parse(text);
    assert.equal(entries.length, 55);
    assert.ok(entries.every((e) => e.type === "Error" && e.board === "00000001"));
  });
});
