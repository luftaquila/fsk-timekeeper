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
