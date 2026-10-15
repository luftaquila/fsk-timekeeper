import { describe, it, expect } from "vitest";
import { zipSync, strToU8 } from "fflate";
import { readFirmwarePackage, compareVersions, METADATA_NAME } from "../src/firmware/package";
import { firmwareFixture } from "./firmware-fixture";

describe("firmware package validation", () => {
  it("accepts direct and release ZIPs with identical metadata", async () => {
    const f = firmwareFixture();
    const direct = await readFirmwarePackage(f.zip());
    expect(direct.metadata.version).toBe("v1.2.3");
    expect(direct.compatible).toBe(true);
    const outer = zipSync({ "fsk-timekeeper-firmware/fsk-timekeeper-dfu.zip": f.zip(),
      [`fsk-timekeeper-firmware/${METADATA_NAME}`]: f.files[METADATA_NAME], "fsk-timekeeper-firmware/flash.sh": strToU8("unused") });
    expect((await readFirmwarePackage(outer)).binary).toEqual(f.binary);
  });
  it("rejects old ZIPs without metadata", async () => {
    const f = firmwareFixture(); delete f.files[METADATA_NAME];
    await expect(readFirmwarePackage(f.zip())).rejects.toThrow(METADATA_NAME);
  });
  it("rejects byte corruption before any serial connection", async () => {
    const f = firmwareFixture(); f.binary[700] ^= 1;
    await expect(readFirmwarePackage(f.zip())).rejects.toThrow("integrity");
  });
  it("rejects altered metadata even if the binary hash is unchanged", async () => {
    const f = firmwareFixture(); f.metadata.version = "v9.9.9";
    f.files[METADATA_NAME] = strToU8(JSON.stringify(f.metadata));
    await expect(readFirmwarePackage(f.zip())).rejects.toThrow("differs from");
  });
  it("rejects mismatched outer metadata and multiple DFU packages", async () => {
    const f = firmwareFixture();
    await expect(readFirmwarePackage(zipSync({ "fsk-timekeeper-dfu.zip": f.zip(), [METADATA_NAME]: strToU8("{}") }))).rejects.toThrow("do not match");
    await expect(readFirmwarePackage(zipSync({ "a/fsk-timekeeper-dfu.zip": f.zip(), "b/fsk-timekeeper-dfu.zip": f.zip() }))).rejects.toThrow("More than one");
  });
  it("refuses other products and bootloader/SoftDevice updates", async () => {
    const f = firmwareFixture();
    f.metadata.product = "OTHER"; f.files[METADATA_NAME] = strToU8(JSON.stringify(f.metadata));
    await expect(readFirmwarePackage(f.zip())).rejects.toThrow("not a supported");
    const other = firmwareFixture(); other.files["manifest.json"] = strToU8(JSON.stringify({ manifest: { application: {}, bootloader: {} } }));
    await expect(readFirmwarePackage(other.zip())).rejects.toThrow("Only application");
  });
  it("identifies protocol incompatibility while retaining package version", async () => {
    const f = await readFirmwarePackage(firmwareFixture({ usb_protocol: 99 }).zip());
    expect(f.compatible).toBe(false);
    expect(f.metadata.usb_protocol).toBe(99);
  });
  it("rejects traversal names and truncated archives", async () => {
    await expect(readFirmwarePackage(zipSync({ "../x.json": strToU8("{}") }))).rejects.toThrow("Invalid");
    await expect(readFirmwarePackage(new Uint8Array([1, 2, 3]))).rejects.toThrow("ZIP");
  });
  it("orders numeric versions and prereleases for reinstall/downgrade", () => {
    expect(compareVersions("v1.10.0", "v1.9.0")).toBe(1);
    expect(compareVersions("1.2.3", "v1.2.3+test")).toBe(0);
    expect(compareVersions("v1.2.3-rc.2", "v1.2.3-rc.10")).toBe(-1);
    expect(compareVersions("v1.2.3-rc.1", "v1.2.3")).toBe(-1);
    expect(compareVersions("bad", "v1.2.3")).toBe(null);
  });
});
