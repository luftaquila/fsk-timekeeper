import { createHash } from "node:crypto";
import { zipSync, strToU8 } from "fflate";
import { crc16 } from "../src/firmware/protocol";
import { METADATA_NAME } from "../src/firmware/package";

export function firmwareFixture({ version = "v1.2.3", usb_protocol = 7, radio_protocol = 15 } = {}) {
  const identity = { schema_version: 1, product: "FSK-WL", board: "supermini-nrf52840", version,
    usb_protocol, radio_protocol, app_start: 0x26000, app_limit: 0xf2000, commit: "a".repeat(40) };
  const binary = new Uint8Array(1024);
  new DataView(binary.buffer).setUint32(0, 0x20040000, true);
  new DataView(binary.buffer).setUint32(4, 0x26101, true);
  binary.set(strToU8("FSK-FIRMWARE-METADATA:" + JSON.stringify(identity) + "\0"), 64);
  const init = Uint8Array.from([0x52, 0, 255, 255, 255, 255, 255, 255, 1, 0, 254, 255, crc16(binary) & 255, crc16(binary) >> 8]);
  const files = { "fsk-timekeeper.bin": binary, "fsk-timekeeper.dat": init };
  const metadata = { ...identity, files: Object.fromEntries(Object.entries(files).map(([name, bytes]) => [name,
    { size: bytes.length, sha256: createHash("sha256").update(bytes).digest("hex") }])) };
  files[METADATA_NAME] = strToU8(JSON.stringify(metadata));
  files["manifest.json"] = strToU8(JSON.stringify({ manifest: { dfu_version: 0.5,
    application: { bin_file: "fsk-timekeeper.bin", dat_file: "fsk-timekeeper.dat" } } }));
  return { identity, binary, init, metadata, files, zip: () => zipSync(files) };
}
