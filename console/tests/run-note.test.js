// @vitest-environment jsdom
import { afterEach, beforeEach, describe, it } from "vitest";
import assert from "node:assert/strict";
import { mount } from "@vue/test-utils";
import { createPinia, disposePinia } from "pinia";
import TimingView from "../src/views/TimingView.vue";

let wrapper;
let pinia;

function openPage() {
  pinia = createPinia();
  wrapper = mount(TimingView, { global: { plugins: [pinia] } });
}

function closePage() {
  wrapper?.unmount();
  if (pinia) disposePinia(pinia);
  wrapper = null;
  pinia = null;
}

beforeEach(() => {
  localStorage.clear();
  localStorage.setItem("tk.settings.v2", JSON.stringify({ lastNote: "Previous run" }));
});
afterEach(closePage);

describe("run note persistence", () => {
  it.each(["", "Next run"])("keeps the edited note %j after reloading without starting a run", async (note) => {
    openPage();
    assert.equal(wrapper.find("input.note").element.value, "Previous run");
    await wrapper.find("input.note").setValue(note);
    assert.equal(JSON.parse(localStorage.getItem("tk.settings.v2")).lastNote, note);

    closePage();
    openPage();
    assert.equal(wrapper.find("input.note").element.value, note);
    assert.equal(wrapper.find(".run-note").exists(), note !== "");
    if (note) assert.equal(wrapper.find(".run-note").text(), note);
  });
});
