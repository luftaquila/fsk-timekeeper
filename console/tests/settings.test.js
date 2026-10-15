// @vitest-environment jsdom
import { beforeEach, describe, it } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";
import { nextTick } from "vue";
import { useSettingsStore } from "../src/stores/settings";
import { missingRoles } from "../src/lib/quality";

beforeEach(() => {
  localStorage.clear();
  setActivePinia(createPinia());
});

describe("sensor participation by role", () => {
  it("persists removal without restoring the old role or note when rediscovered", async () => {
    const settings = useSettingsStore();
    settings.setMapping("00000001", { role: "start", note: "old gate" });
    settings.removeSensor("00000001");
    settings.removeSensor("00000001");
    settings.removeSensor("0");
    await nextTick();
    settings.$dispose(); setActivePinia(createPinia());
    const restored = useSettingsStore();
    assert.equal(restored.mappingOf("00000001"), null);
    assert.deepEqual(restored.state.removedSensors, ["00000001"]);
    restored.restoreSensor("00000001");
    assert.deepEqual(restored.state.removedSensors, []);
    assert.equal(restored.mappingOf("00000001"), null);
  });
  it("migrates disabled sensors to unused without losing notes or active roles", () => {
    localStorage.setItem("tk.settings.v2", JSON.stringify({ mapping: {
      "00000001": { role: "start", note: "spare", enabled: false },
      "00000002": { role: "finish", note: "finish line", enabled: true },
      "00000003": { role: "start", note: "start line" },
    } }));
    const settings = useSettingsStore();
    assert.deepEqual(settings.mappingOf("00000001"), { node_id: "00000001", role: "unused", note: "spare" });
    assert.deepEqual(settings.mappingsFor("sprint").map(m => m.node_id), ["00000002", "00000003"]);
    assert.deepEqual(settings.mappingsFor("laps").map(m => m.node_id), ["00000003"]);
  });

  it("excludes unused sensors from required roles and preserves the choice across reload", async () => {
    const settings = useSettingsStore();
    settings.setMapping("00000001", { role: "start", note: "gate" });
    settings.setMapping("00000002", { role: "finish" });
    assert.deepEqual(missingRoles("sprint", settings.mappingsFor("sprint")), []);
    settings.setMapping("00000001", { role: "unused", note: "gate" });
    assert.deepEqual(missingRoles("sprint", settings.mappingsFor("sprint")), ["start"]);
    assert.deepEqual(settings.mappingsFor("laps"), []);
    await nextTick();
    settings.$dispose();
    setActivePinia(createPinia());
    const restored = useSettingsStore();
    assert.equal(restored.mappingOf("00000001").role, "unused");
    assert.equal(restored.mappingOf("00000001").note, "gate");
    restored.setMapping("00000001", { role: "start", note: "gate" });
    assert.deepEqual(missingRoles("sprint", restored.mappingsFor("sprint")), []);
  });
});
