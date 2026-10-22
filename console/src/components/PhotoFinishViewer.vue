<script setup>
/* Photo-finish image of one camera crossing: lines (time) run left to right, or right to left for
 * cars moving right as seen from the camera (meta.mirror), so the car faces its way. Full-resolution
 * tiles from the camera Pi, else the clip kept in the browser. The cursor reads the time of its
 * line within the crossing's exactly timed run; the operator may pick the front line there. */
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useCameraStore } from "../stores/camera";
import { useTimingStore } from "../stores/timing";
import { lineTick, pickableLines, carText, carNumberOf } from "../lib/camera";
import { spanNs } from "../lib/results";
import { formatDuration } from "../lib/format";
import { nominalNs } from "../lib/event-timing";

const props = defineProps({
  crossing: { type: Object, required: true }, // engine crossing with key, autoTick, camera
  run: { type: Object, default: null }, // its run: time reference and decisions
  refTick: { type: String, default: null }, // times shown since this tick
  editable: { type: Boolean, default: false },
  showCar: { type: Boolean, default: true },
});
const emit = defineEmits(["decide"]);

const camera = useCameraStore();
const timing = useTimingStore();
const canvas = ref(null);
const meta = ref(null);
const error = ref("");
const zoom = ref(null); // lines per screen pixel
const offset = ref(0); // first line at the left edge
const cursor = ref(null);
const contrast = ref(100);
const brightness = ref(100);
const images = new Map(); // url -> HTMLImageElement
let previewUrl = null;
let previewImage = null;
let drag = null;

const RULER = 22;
const HEIGHT = 360;
const row = computed(() => ({ master_tick: props.crossing.autoTick ?? props.crossing.tick, camera: props.crossing.camera }));
const decision = computed(() => props.run?.decisions?.[props.crossing.key] ?? null);
const autoLine = computed(() => BigInt(props.crossing.camera.line));
const frontLine = computed(() => (decision.value?.line != null ? BigInt(decision.value.line) : autoLine.value));
const pickable = computed(() => pickableLines(props.crossing.camera));
const mirror = computed(() => !!meta.value?.mirror);

function tickOf(line) {
  return lineTick(row.value, line);
}

// Time since refTick at a tick: calibrated like the results, nominal 16 MHz without a run.
function since(tick) {
  if (tick == null || props.refTick == null) return null;
  if (props.run) {
    const ns = spanNs(props.run, props.refTick, String(tick), timing.timeline);
    if (ns != null) return ns;
  }
  return nominalNs(BigInt(tick) - BigInt(props.refTick));
}

const cursorText = computed(() => {
  if (cursor.value == null) return "";
  const t = tickOf(cursor.value);
  if (t == null) return `Line ${cursor.value} · no exact time`;
  const ns = since(t);
  return ns == null ? `Line ${cursor.value}` : `Line ${cursor.value} · ${formatDuration(ns)}`;
});
const frontText = computed(() => {
  const ns = since(tickOf(frontLine.value));
  return ns == null ? "-" : formatDuration(ns);
});

function width() {
  return canvas.value?.clientWidth || 800;
}

function fit() {
  if (!meta.value) return;
  const lines = meta.value.last - meta.value.first + 1;
  zoom.value = Math.max(lines / width(), 0.05);
  offset.value = meta.value.first;
  draw();
}

function focusFront() {
  if (!meta.value) return;
  zoom.value = 1;
  offset.value = Number(frontLine.value) - width() / 2;
  draw();
}

// Screen x <-> x in line order (the same with no mirror).
function sx(x) {
  return mirror.value ? width() - x : x;
}
function lineAt(x) {
  return Math.round(offset.value + sx(x) * zoom.value);
}
// x of a line in line order; draw() mirrors the canvas for it.
function xOf(line) {
  return (Number(line) - offset.value) / zoom.value;
}

function image(url) {
  if (!url) return null;
  let img = images.get(url);
  if (!img) {
    img = new Image();
    img.onload = draw;
    img.onerror = () => { error.value = "Image unavailable"; };
    img.src = url;
    images.set(url, img);
  }
  return img.complete && img.naturalWidth ? img : null;
}

function draw() {
  const el = canvas.value;
  const ctx = el?.getContext?.("2d");
  const m = meta.value;
  if (!ctx || !m || !zoom.value) return;
  const w = (el.width = el.clientWidth || 800);
  const h = (el.height = HEIGHT);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#000";
  ctx.fillRect(0, RULER, w, h - RULER);
  ctx.imageSmoothingEnabled = zoom.value > 1;
  const top = RULER;
  const rows = h - RULER;
  ctx.save();
  if (mirror.value) {
    ctx.translate(w, 0);
    ctx.scale(-1, 1);
  }
  const from = Math.max(m.first, Math.floor(offset.value));
  const to = Math.min(m.last, Math.ceil(offset.value + w * zoom.value));
  if (!m.stored && camera.connected) {
    for (let i = Math.floor((from - m.first) / m.tileLines); i * m.tileLines + m.first <= to && i < m.tiles; i++) {
      const img = image(camera.tileUrl(props.crossing.key, i));
      if (!img) continue;
      const first = m.first + i * m.tileLines;
      ctx.drawImage(img, 0, 0, img.naturalWidth, img.naturalHeight, xOf(first), top, img.naturalWidth / zoom.value, rows);
    }
  } else if (previewImage?.complete && previewImage.naturalWidth) {
    const per = m.preview?.linesPerPixel || 1;
    ctx.drawImage(previewImage, 0, 0, previewImage.naturalWidth, previewImage.naturalHeight,
      xOf(m.first), top, (previewImage.naturalWidth * per) / zoom.value, rows);
  }
  // Lines outside the exactly timed run: shaded.
  ctx.fillStyle = "rgba(127,127,127,0.35)";
  const lo = xOf(pickable.value.first);
  const hi = xOf(pickable.value.last + 1n);
  if (lo > 0) ctx.fillRect(0, top, Math.min(lo, w), rows);
  if (hi < w) ctx.fillRect(Math.max(hi, 0), top, w - Math.max(hi, 0), rows);
  marker(ctx, autoLine.value, "#ef4444", [6, 4], h);
  if (decision.value?.line != null) marker(ctx, frontLine.value, "#22c55e", [], h);
  if (cursor.value != null) marker(ctx, BigInt(cursor.value), "rgba(255,255,255,0.8)", [2, 3], h);
  ctx.restore();
  ruler(ctx, w);
}

function marker(ctx, line, color, dash, h) {
  const x = xOf(line) + 0.5 / zoom.value;
  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.setLineDash(dash);
  ctx.beginPath();
  ctx.moveTo(x, RULER);
  ctx.lineTo(x, h);
  ctx.stroke();
  ctx.restore();
}

// Time ticks every 1, 2 or 5 x 10^k ms of lines, labelled since refTick.
function ruler(ctx, w) {
  ctx.fillStyle = getComputedStyle(canvas.value).getPropertyValue("--bg-secondary") || "#1f2937";
  ctx.fillRect(0, 0, w, RULER);
  const period = props.crossing.camera.period;
  const linesPerMs = 16000 / period;
  const targetMs = (90 * zoom.value) / linesPerMs;
  const step = [1, 2, 5].map((k) => k * 10 ** Math.floor(Math.log10(Math.max(targetMs, 0.01)))).find((s) => s >= targetMs) ?? 10 ** Math.ceil(Math.log10(targetMs));
  const front = Number(frontLine.value);
  ctx.fillStyle = getComputedStyle(canvas.value).getPropertyValue("--text-secondary") || "#cbd5e1";
  ctx.font = "11px monospace";
  const startMs = Math.floor(((offset.value - front) / linesPerMs) / step) * step;
  for (let ms = startMs; (front + ms * linesPerMs - offset.value) / zoom.value < w; ms += step) {
    const x = sx((front + ms * linesPerMs - offset.value) / zoom.value);
    ctx.fillRect(x, RULER - 6, 1, 6);
    ctx.fillText(`${ms > 0 ? "+" : ""}${Number(ms.toFixed(3))} ms`, x + 3, 12);
  }
}

function onMove(e) {
  const x = e.offsetX;
  if (drag) {
    offset.value = drag.offset - (sx(x) - sx(drag.x)) * zoom.value;
    drag.moved ||= Math.abs(x - drag.x) > 3;
  }
  cursor.value = lineAt(x);
  draw();
}
function onDown(e) {
  drag = { x: e.offsetX, offset: offset.value, moved: false };
}
function onUp(e) {
  const clicked = drag && !drag.moved;
  drag = null;
  if (!clicked || !props.editable) return;
  const line = lineAt(e.offsetX);
  if (tickOf(line) != null) emit("decide", { line });
}
function onLeave() {
  drag = null;
  cursor.value = null;
  draw();
}
function onWheel(e) {
  e.preventDefault();
  const at = lineAt(e.offsetX);
  const lines = meta.value ? meta.value.last - meta.value.first + 1 : 1e6;
  zoom.value = Math.min(Math.max(zoom.value * (e.deltaY > 0 ? 1.25 : 0.8), 0.05), lines / 50);
  offset.value = at - sx(e.offsetX) * zoom.value;
  draw();
}

function nudge(delta) {
  const line = frontLine.value + BigInt(delta);
  if (tickOf(line) != null) emit("decide", { line: String(line) });
}

const carInput = ref("");
function saveCar() {
  const n = carNumberOf(carInput.value);
  emit("decide", { car: n || null });
}

function exportPng() {
  canvas.value?.toBlob?.((blob) => {
    if (!blob) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `photofinish-${props.crossing.key.replaceAll(":", "-")}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
}

async function load() {
  error.value = "";
  meta.value = null;
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = null;
  previewImage = null;
  try {
    meta.value = await camera.clip(props.crossing.key);
    if (meta.value.stored || !camera.connected) {
      const stored = await camera.preview(props.crossing.key);
      if (stored?.image) {
        previewUrl = URL.createObjectURL(stored.image);
        previewImage = new Image();
        previewImage.onload = draw;
        previewImage.src = previewUrl;
      }
    }
    fit();
  } catch (e) {
    error.value = e.message || "Clip unavailable";
  }
}

watch(() => props.crossing.key, load);
watch(() => [decision.value, contrast.value, brightness.value], draw, { deep: true });
watch(() => props.crossing.camera.car, () => { carInput.value = props.crossing.car ?? carText(props.crossing.camera.car) ?? ""; }, { immediate: true });
let resize;
onMounted(() => {
  load();
  resize = () => draw();
  window.addEventListener("resize", resize);
});
onBeforeUnmount(() => {
  window.removeEventListener("resize", resize);
  if (previewUrl) URL.revokeObjectURL(previewUrl);
});
</script>

<template>
  <div class="viewer">
    <div class="viewer-head">
      <span class="mono front">Front {{ frontText }}</span>
      <span v-if="decision?.line != null" class="badge badge-success">Operator Line</span>
      <span v-if="decision?.ignore" class="badge badge-default">Not a Vehicle</span>
      <span v-if="showCar" class="mono car">Car {{ crossing.car ?? carText(crossing.camera.car) ?? "?" }}
        <small>OCR {{ carText(crossing.camera.car) ?? "-" }} · {{ crossing.camera.conf }}%</small></span>
      <span class="spacer" />
      <span class="mono cursor-text">{{ cursorText }}</span>
    </div>
    <div v-if="error" class="empty-state">{{ error }}</div>
    <canvas v-show="!error" ref="canvas" class="image" :style="{ filter: `contrast(${contrast}%) brightness(${brightness}%)` }"
      @mousemove="onMove" @mousedown="onDown" @mouseup="onUp" @mouseleave="onLeave" @wheel="onWheel" />
    <div class="viewer-tools">
      <button class="btn btn-ghost btn-sm" type="button" @click="fit">Fit</button>
      <button class="btn btn-ghost btn-sm" type="button" @click="focusFront">1:1 at Front</button>
      <label class="slider">Contrast <input v-model.number="contrast" type="range" min="50" max="300" /></label>
      <label class="slider">Brightness <input v-model.number="brightness" type="range" min="50" max="300" /></label>
      <button class="btn btn-ghost btn-sm" type="button" @click="exportPng">PNG</button>
      <template v-if="editable">
        <span class="spacer" />
        <button class="btn btn-ghost btn-sm" type="button" title="One line earlier" @click="nudge(-1)">◀</button>
        <button class="btn btn-ghost btn-sm" type="button" title="One line later" @click="nudge(1)">▶</button>
        <button class="btn btn-ghost btn-sm" type="button" :disabled="decision?.line == null" @click="emit('decide', { line: null })">Detector Line</button>
        <label v-if="showCar" class="car-edit">Car <input v-model="carInput" class="form-input mono" inputmode="numeric" maxlength="3" @change="saveCar" /></label>
        <button class="btn btn-sm" :class="decision?.ignore ? 'btn-danger' : 'btn-ghost'" type="button"
          @click="emit('decide', { ignore: !decision?.ignore })">Not a Vehicle</button>
      </template>
    </div>
    <p v-if="editable" class="hint">Click the image to set the front line. Gray: no exact time.</p>
  </div>
</template>

<style scoped>
.viewer { display: grid; gap: 0.5rem; }
.viewer-head, .viewer-tools { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.spacer { flex: 1; }
.front { font-weight: 700; font-size: 1.1rem; }
.car small { color: var(--text-tertiary); margin-left: 0.35rem; }
.cursor-text { color: var(--text-secondary); }
.image { width: 100%; height: 360px; border-radius: 8px; background: #000; cursor: crosshair; touch-action: none; }
.slider { display: inline-flex; align-items: center; gap: 0.35rem; color: var(--text-secondary); }
.slider input { width: 90px; }
.car-edit { display: inline-flex; align-items: center; gap: 0.35rem; }
.car-edit input { width: 4.5rem; }
.hint { color: var(--text-tertiary); margin: 0; }
</style>
