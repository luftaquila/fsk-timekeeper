/* Camera Pi link: the master over the Pi bridge (device store, transport "pi"), the Pi's own
 * status and settings, clip metadata, and the downscaled clips kept for History. */
import { defineStore } from "pinia";
import { ref, computed, watch } from "vue";
import { useDeviceStore } from "./device";
import { useSettingsStore } from "./settings";
import { useTimingStore } from "./timing";
import { cameraApi, cameraContract } from "../lib/cameraApi";
import { USB_PROTO_VERSION } from "../lib/protocol";
import * as photos from "../lib/photos";

const INFO_PERIOD_MS = 5000;

// "<node>:<session>:<seq>" -> { session, seq }
export function clipOf(key) {
  const [, session, seq] = String(key).split(":");
  return { session, seq: Number(seq) };
}

export const useCameraStore = defineStore("camera", () => {
  const info = ref(null);
  const infoError = ref(null);
  const config = ref(null); // settings kept on the Pi: { values, limits: { [key]: { default, min, max } } }
  const clips = new Map(); // key -> Promise<meta>
  const saving = new Set();
  let timer = null;

  const device = useDeviceStore();
  const settings = useSettingsStore();
  const connected = computed(() => device.connected && device.transportKind === "pi");
  const api = computed(() => (settings.state.cameraHost ? cameraApi(settings.state.cameraHost) : null));
  const contract = computed(() => cameraContract(info.value, USB_PROTO_VERSION));

  async function refresh() {
    if (!api.value) return;
    try {
      info.value = await api.value.info();
      infoError.value = null;
    } catch (error) {
      info.value = null;
      infoError.value = error.message || String(error);
    }
  }

  function poll(on) {
    clearInterval(timer);
    timer = null;
    if (!on) return;
    refresh();
    timer = setInterval(refresh, INFO_PERIOD_MS);
  }
  watch(connected, poll);

  async function loadConfig() {
    config.value = null;
    if (!connected.value || !api.value) return;
    try {
      config.value = await api.value.config();
    } catch {
      config.value = null;
    }
  }
  watch(connected, loadConfig);

  // A change restarts the Pi's camera session with the new settings.
  async function saveConfig(patch) {
    config.value = await api.value.setConfig(patch);
    await refresh();
  }

  async function connect() {
    await refresh();
    if (!contract.value.ok) throw new Error(infoError.value || contract.value.reason);
    return device.connect({ kind: "pi", host: settings.state.cameraHost });
  }

  // Clip metadata from the Pi (cached per key), else the stored copy's.
  async function clip(key) {
    if (!clips.has(key)) {
      const { session, seq } = clipOf(key);
      const fetching = api.value ? api.value.clip(session, seq) : Promise.reject(new Error("No camera host"));
      clips.set(key, fetching.catch(async (error) => {
        clips.delete(key);
        const stored = await photos.getPhoto(photos.photoKey(session, seq));
        if (stored) return { ...stored.meta, stored: true };
        throw error;
      }));
    }
    return clips.get(key);
  }

  function tileUrl(key, i) {
    const { session, seq } = clipOf(key);
    return api.value?.tileUrl(session, seq, i) ?? null;
  }

  // The downscaled clip: the stored copy, else the Pi's (kept for History once fetched).
  async function preview(key) {
    const { session, seq } = clipOf(key);
    const stored = await photos.getPhoto(photos.photoKey(session, seq));
    if (stored) return stored;
    return keep(key);
  }

  async function keep(key) {
    const { session, seq } = clipOf(key);
    if (!api.value || saving.has(key)) return null;
    saving.add(key);
    try {
      const [meta, image] = await Promise.all([clip(key), api.value.preview(session, seq)]);
      return await photos.savePhoto({ session, seq, meta, image });
    } catch {
      return null; // the Pi may still be writing the clip; the next evaluation retries
    } finally {
      saving.delete(key);
    }
  }

  // Keep the clips of the selected run's camera crossings.
  const timing = useTimingStore();
  watch(() => timing.live.crossings, async (crossings) => {
    if (!connected.value) return;
    for (const c of crossings || []) {
      if (!c.key) continue;
      const { session, seq } = clipOf(c.key);
      if (!(await photos.getPhoto(photos.photoKey(session, seq)))) keep(c.key);
    }
  });

  return { info, infoError, config, connected, api, contract, refresh, connect, loadConfig, saveConfig, clip, tileUrl, preview };
});
