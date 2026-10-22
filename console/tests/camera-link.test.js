// @vitest-environment jsdom
// Camera Pi link: the WebSocket transport, the API contract, camera settings.
import { describe, it, beforeEach } from "vitest";
import assert from "node:assert/strict";
import { createPinia, setActivePinia } from "pinia";
import { createPiTransport, PI_BUSY_CLOSE } from "../src/transport/pi.js";
import { cameraContract, CAMERA_API } from "../src/lib/cameraApi.js";
import { cameraNodeOf, USB_PROTO_VERSION } from "../src/lib/protocol.js";

class FakeSocket {
  static last = null;
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    FakeSocket.last = this;
  }
  send(data) { this.sent.push(data); }
  close() { this.readyState = 3; this.onclose?.({ code: 1000 }); }
  accept() { this.readyState = 1; this.onopen?.(); }
  refuse(code) { this.readyState = 3; this.onclose?.({ code }); }
}

describe("Pi transport", () => {
  it("relays lines both ways and reports an unexpected close", async () => {
    const lines = [];
    let disconnected = 0;
    const t = createPiTransport({ onLine: (l) => lines.push(l), onDisconnect: () => disconnected++ }, { host: "cam.tail.ts.net", WebSocketImpl: FakeSocket });
    const opening = t.open();
    assert.equal(FakeSocket.last.url, "wss://cam.tail.ts.net/api/master");
    FakeSocket.last.accept();
    await opening;
    assert.equal(t.connected, true);
    FakeSocket.last.onmessage({ data: "H 1 2 3 0" });
    FakeSocket.last.onmessage({ data: "A V OK\nX camwait" });
    assert.deepEqual(lines, ["H 1 2 3 0", "A V OK", "X camwait"]);
    assert.equal(await t.write("?ID"), true);
    assert.deepEqual(FakeSocket.last.sent, ["?ID"]);
    FakeSocket.last.readyState = 3;
    FakeSocket.last.onclose({ code: 1006 });
    assert.equal(disconnected, 1);
    assert.equal(t.connected, false);
    assert.equal(await t.write("?ID"), false);
  });

  it("names a busy bridge and stays silent on its own close", async () => {
    const t = createPiTransport({ onDisconnect: () => assert.fail("own close") }, { host: "cam", WebSocketImpl: FakeSocket });
    const busy = t.open();
    FakeSocket.last.refuse(PI_BUSY_CLOSE);
    await assert.rejects(busy, /Another console/);
    const ok = t.open();
    FakeSocket.last.accept();
    await ok;
    await t.close();
    await assert.rejects(t.enterBootloader(), /over USB/);
  });
});

describe("camera contract", () => {
  it("needs the console's API and USB versions", () => {
    assert.equal(cameraContract(null, USB_PROTO_VERSION).ok, false);
    assert.equal(cameraContract({ api: CAMERA_API, usbProto: USB_PROTO_VERSION }, USB_PROTO_VERSION).ok, true);
    assert.equal(cameraContract({ api: CAMERA_API + 1, usbProto: USB_PROTO_VERSION }, USB_PROTO_VERSION).ok, false);
    assert.equal(cameraContract({ api: CAMERA_API, usbProto: USB_PROTO_VERSION - 1 }, USB_PROTO_VERSION).ok, false);
  });

  it("takes the camera node from the master's devid", () => {
    assert.equal(cameraNodeOf({ devid: "0123ABCD89EF4567" }), "89EF4567");
    assert.equal(cameraNodeOf({ devid: "0123ABCD00000000" }), null);
    assert.equal(cameraNodeOf({ devid: "xyz" }), null);
    assert.equal(cameraNodeOf(null), null);
  });
});

describe("camera settings", () => {
  beforeEach(() => {
    localStorage.clear();
    setActivePinia(createPinia());
  });

  it("keeps the host bare and persists the camera settings and a Photo mode", async () => {
    const { useSettingsStore } = await import("../src/stores/settings.js");
    const settings = useSettingsStore();
    settings.setCameraHost("https://cam.tail-1234.ts.net/");
    assert.equal(settings.state.cameraHost, "cam.tail-1234.ts.net");
    assert.throws(() => settings.setCameraHost("cam host"), /host name/);
    settings.setOcrMinConf("150");
    assert.equal(settings.state.ocrMinConf, 100);
    assert.throws(() => settings.setMode("photo"), /Invalid mode/);
    settings.setMode("camera-sprint");
    await new Promise((r) => setTimeout(r, 0));
    setActivePinia(createPinia());
    const again = useSettingsStore();
    assert.equal(again.state.cameraHost, "cam.tail-1234.ts.net");
    assert.equal(again.state.ocrMinConf, 100);
    assert.equal(again.state.mode, "camera-sprint");
    assert.equal(again.state.cars, undefined);
  });
});
