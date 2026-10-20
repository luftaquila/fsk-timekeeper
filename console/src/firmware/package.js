import { unzipSync, strFromU8 } from "fflate";
import { RADIO_PROTO_VERSION, USB_PROTO_VERSION } from "../lib/protocol";
import { crc16 } from "./protocol";

export const METADATA_NAME = "fsk-timekeeper-metadata.json";
const DFU_NAME = "fsk-timekeeper-dfu.zip";
const LIMIT = 32 * 1024 * 1024;
const VERSION = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([\da-zA-Z.-]+))?(?:\+[\da-zA-Z.-]+)?$/;
const fail = (message) => { throw new Error(message); };

function unpack(bytes) {
  if (!bytes.length || bytes.length > LIMIT) fail("ZIP file is empty or too large (maximum 32 MB).");
  const seen = new Set();
  let total = 0;
  try {
    return unzipSync(bytes, { filter: (entry) => {
      const name = entry.name;
      total += entry.originalSize;
      if (seen.has(name) || /(^\/|\\|(^|\/)\.\.?($|\/))/.test(name) || total > LIMIT * 2 || entry.originalSize > LIMIT) {
        fail("Invalid or oversized ZIP contents.");
      }
      seen.add(name);
      return /\.(json|bin|dat)$/.test(name) || name.endsWith(DFU_NAME);
    } });
  } catch (e) { fail(`Cannot read firmware ZIP: ${e.message}`); }
}

function json(bytes, name) {
  if (!bytes || bytes.length > 16384) fail(`Missing or invalid ${name}.`);
  try { return JSON.parse(strFromU8(bytes)); } catch { fail(`Invalid ${name}.`); }
}

function checkIdentity(m) {
  if (m?.schema_version !== 1 || m.product !== "FSK-WL" || m.board !== "supermini-nrf52840" ||
      m.app_start !== 0x26000 || m.app_limit !== 0xf2000 || !VERSION.test(m.version) ||
      !/^[0-9a-f]{40}$/.test(m.commit) || !Number.isInteger(m.usb_protocol) || !Number.isInteger(m.radio_protocol)) {
    fail("This package is not a supported FSK Timekeeper firmware package.");
  }
}

export async function readFirmwarePackage(bytes) {
  const outer = unpack(bytes);
  const nested = Object.keys(outer).filter((n) => n.split("/").at(-1) === DFU_NAME);
  let files = outer;
  let outerMetadata;
  if (nested.length) {
    if (nested.length !== 1) fail("More than one DFU package was found.");
    const prefix = nested[0].slice(0, -DFU_NAME.length);
    outerMetadata = outer[prefix + METADATA_NAME];
    json(outerMetadata, METADATA_NAME);
    files = unpack(outer[nested[0]]);
  }
  const metadata = json(files[METADATA_NAME], METADATA_NAME);
  checkIdentity(metadata);
  if (outerMetadata && strFromU8(outerMetadata) !== strFromU8(files[METADATA_NAME])) {
    fail("The outer ZIP and DFU ZIP metadata do not match.");
  }
  const manifest = json(files["manifest.json"], "DFU manifest").manifest;
  if (!manifest || Object.keys(manifest).length !== 2 || !manifest.application || manifest.dfu_version !== 0.5) fail("Only application firmware upgrades are supported.");
  const app = manifest.application;
  if (!/^[\w.-]+\.bin$/.test(app.bin_file) || !/^[\w.-]+\.dat$/.test(app.dat_file)) fail("Invalid DFU file names.");
  const binary = files[app.bin_file];
  const init = files[app.dat_file];
  if (!binary || binary.length < 8 || binary.length % 4 || binary.length > 0xcc000 || !init || init.length > 256) fail("Invalid firmware size.");
  if (!metadata.files || Object.keys(metadata.files).length !== 2) fail("Invalid firmware file metadata.");
  for (const name of [app.bin_file, app.dat_file]) {
    const description = metadata.files[name];
    const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", files[name])), b => b.toString(16).padStart(2, "0")).join("");
    if (description?.size !== files[name].length || description?.sha256 !== digest) fail(`Firmware integrity check failed: ${name}.`);
  }
  // This identity is linked into the image, not guessed from arbitrary version strings.
  const text = new TextDecoder("latin1").decode(binary);
  const marker = "FSK-FIRMWARE-METADATA:";
  const start = text.indexOf(marker);
  if (start < 0 || text.indexOf(marker, start + marker.length) !== -1) fail("Missing embedded firmware identity.");
  let embedded;
  try { embedded = JSON.parse(text.slice(start + marker.length, text.indexOf("\0", start))); }
  catch { fail("Invalid embedded firmware identity."); }
  checkIdentity(embedded);
  for (const key of Object.keys(embedded)) {
    if (metadata[key] !== embedded[key]) fail("Package metadata differs from the firmware image.");
  }
  const vector = new DataView(binary.buffer, binary.byteOffset, binary.byteLength);
  const stack = vector.getUint32(0, true), reset = vector.getUint32(4, true);
  if (stack < 0x20000000 || stack > 0x20040000 || stack % 8 || !(reset & 1) ||
      reset < metadata.app_start || reset >= metadata.app_start + binary.length) fail("Invalid application vector table.");
  // Legacy Adafruit init packet: device/revision/version, SD list, CRC16.
  const dat = new DataView(init.buffer, init.byteOffset, init.byteLength);
  if (init.length < 12 || dat.getUint16(0, true) !== 0x52 ||
      init.length !== 12 + 2 * dat.getUint16(8, true) ||
      dat.getUint16(init.length - 2, true) !== crc16(binary)) fail("DFU initialization data does not match the firmware.");
  const compatible = metadata.usb_protocol === USB_PROTO_VERSION && metadata.radio_protocol === RADIO_PROTO_VERSION;
  return { metadata, binary, init, compatible };
}

// Semantic ordering, including prereleases; build metadata does not affect precedence.
export function compareVersions(a, b) {
  const left = VERSION.exec(a), right = VERSION.exec(b);
  if (!left || !right) return null;
  for (let i = 1; i <= 3; i++) {
    if (BigInt(left[i]) !== BigInt(right[i])) return BigInt(left[i]) > BigInt(right[i]) ? 1 : -1;
  }
  const x = left[4]?.split("."), y = right[4]?.split(".");
  if (!x || !y) return x ? -1 : y ? 1 : 0;
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if (x[i] === y[i]) continue;
    if (x[i] === undefined) return -1;
    if (y[i] === undefined) return 1;
    const xn = /^\d+$/.test(x[i]), yn = /^\d+$/.test(y[i]);
    if (xn && yn) return BigInt(x[i]) > BigInt(y[i]) ? 1 : -1;
    if (xn !== yn) return xn ? -1 : 1;
    return x[i] > y[i] ? 1 : -1;
  }
  return 0;
}
