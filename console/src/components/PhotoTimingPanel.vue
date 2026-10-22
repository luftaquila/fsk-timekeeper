<script setup>
/* Timing page content of the camera modes: Photo Sprint's start and finish lists, Photo Laps'
 * car table and crossing feed, and the photo-finish image of the selected crossing. */
import { computed, ref, watch } from "vue";
import { useTimingStore } from "../stores/timing";
import { RESULT_CLASSIFICATIONS } from "../lib/constants";
import { formatDuration } from "../lib/format";
import { spanNs } from "../lib/results";
import { carText } from "../lib/camera";
import FaultReasons from "./FaultReasons.vue";
import PhotoFinishViewer from "./PhotoFinishViewer.vue";

const props = defineProps({ mode: { type: String, required: true } });

const timing = useTimingStore();

const run = computed(() => (timing.run?.mode === props.mode ? timing.run : null));
const crossings = computed(() => (run.value ? timing.live.crossings : []));
const cameraCrossings = computed(() => crossings.value.filter((c) => c.key));
const selectedKey = ref(null);
const selected = computed(() => cameraCrossings.value.find((c) => c.key === selectedKey.value) ?? cameraCrossings.value.at(-1) ?? null);
watch(() => run.value?.runId, () => { selectedKey.value = null; });

// Photo Sprint: times since the first start crossing; Photo Laps: since the heat's first crossing.
const refTick = computed(() => crossings.value.find((c) => c.role === "start" && c.state !== "ignored" && c.state !== "unassigned")?.tick ?? crossings.value[0]?.tick ?? null);
function since(tick) {
  if (!run.value || refTick.value == null) return null;
  return spanNs(run.value, refTick.value, tick, timing.timeline);
}

const startList = computed(() => timing.crossingRows("start"));
const finishList = computed(() => timing.crossingRows("finish"));

const sprintStatus = computed(() => {
  const r = run.value;
  if (!r || r.mode !== "camera-sprint") return null;
  if (r.classification) return { cls: r.classification === "DSQ" ? "badge-danger" : "badge-warning", text: r.classification };
  if (r.verification === "verified") return { cls: "badge-success", text: "OK" };
  if (r.verification === "invalid") return { cls: "badge-danger", text: "Invalid" };
  return { cls: "badge-warning", text: r.closed ? "Stopped" : "Pending" };
});

function carStatus(result) {
  if (result.classification) return { cls: result.classification === "DSQ" ? "badge-danger" : "badge-warning", text: result.classification };
  if (result.verification === "invalid") return { cls: "badge-danger", text: "Invalid" };
  if (result.verification === "verified" && result.closed) return { cls: "badge-success", text: "OK" };
  return { cls: "badge-default", text: result.closed ? "Stopped" : "Running" };
}
// Lap times as shown: "?" for a lap an unassigned crossing falls in (it may be two laps).
function lapText(row, i) {
  return i < 0 || i >= row.laps.length ? "-" : row.heldLaps.has(i) ? "?" : formatDuration(row.laps[i]);
}
function bestText(row) {
  if (row.held) return "?";
  return row.laps.length ? formatDuration(row.laps.reduce((a, b) => (b < a ? b : a))) : "-";
}
// Cars of the heat so far, for assigning a crossing; a new number goes in the viewer's Car box.
const cars = computed(() => timing.carRows.map((row) => row.car));

const STATE_LABEL = { assigned: "", unassigned: "Unassigned", ignored: "Not a Vehicle" };
function decide(patch) {
  if (selected.value) timing.decide(selected.value.key, patch);
}
function classify(car, event) {
  timing.classify(event.target.value || null, { runId: run.value.runId, car });
}
</script>

<template>
  <div v-if="sprintStatus || run?.durable === false" class="status-line">
    <span v-if="sprintStatus" class="badge" :class="sprintStatus.cls">{{ sprintStatus.text }}</span>
    <span v-if="sprintStatus && (run.closed || run.verification === 'verified')" class="mono result">{{ formatDuration(timing.resultNs) }}</span>
    <span v-if="run.durable === false" class="not-saved">Not Saved</span>
  </div>
  <FaultReasons v-if="run?.fault" :reasons="run.fault.reasons || []" :boundary-tick="run.boundaryTick" />

  <div v-if="mode === 'camera-sprint'" class="sensors">
    <div v-for="[title, list] in [['🟢 Start', startList], ['🏁 Finish', finishList]]" :key="title" class="card">
      <div class="card-header"><h3>{{ title }}</h3></div>
      <div class="card-body">
        <div v-if="!list.length" class="empty-state">No crossing yet</div>
        <ul v-else class="rec-list">
          <li v-for="c in list" :key="c.key" :class="{ pick: c.camera, active: c.key === selected?.key }" @click="c.camera && (selectedKey = c.key)">
            <span class="mono">{{ formatDuration(c.ns) }}</span>
            <span class="who mono">{{ c.camera ? `📷 ${carText(c.camera.car) ?? ""}` : c.node_id }}</span>
          </li>
        </ul>
      </div>
    </div>
  </div>

  <template v-else-if="run">
    <div class="card">
      <div class="card-header"><h3>🏎️ Cars</h3></div>
      <div class="card-body table-scroll">
        <div v-if="!timing.carRows.length" class="empty-state">No car yet</div>
        <table v-else class="car-table">
          <thead><tr><th>Car</th><th>Laps</th><th>Last</th><th>Best</th><th>Total</th><th>Status</th><th>Decision</th></tr></thead>
          <tbody>
            <tr v-for="row in timing.carRows" :key="row.car">
              <td class="mono">{{ row.car }}</td>
              <td class="mono">{{ row.laps.length }}{{ row.held ? "+?" : "" }}<span v-if="run.lapTarget"> / {{ run.lapTarget }}</span></td>
              <td class="mono">{{ lapText(row, row.laps.length - 1) }}</td>
              <td class="mono">{{ bestText(row) }}</td>
              <td class="mono">{{ row.held && row.result.verification !== "invalid" ? "?" : formatDuration(row.totalNs) }}</td>
              <td><span class="badge" :class="carStatus(row.result).cls">{{ carStatus(row.result).text }}</span></td>
              <td>
                <select class="form-select" :value="row.result.classification || ''" :disabled="timing.classifying" @change="classify(row.car, $event)">
                  <option value="">-</option>
                  <option v-for="v in RESULT_CLASSIFICATIONS" :key="v" :value="v">{{ v }}</option>
                </select>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <div class="card">
      <div class="card-header"><h3>📷 Crossings</h3></div>
      <div class="card-body">
        <div v-if="!cameraCrossings.length" class="empty-state">No crossing yet</div>
        <ul v-else class="rec-list feed">
          <li v-for="c in [...cameraCrossings].reverse()" :key="c.key" class="pick" :class="[c.state, { active: c.key === selected?.key }]" @click="selectedKey = c.key">
            <span class="mono">{{ formatDuration(since(c.tick)) }}</span>
            <span class="mono car">{{ c.car ?? "???" }}</span>
            <span class="ocr">OCR {{ carText(c.camera.car) ?? "-" }} · {{ c.camera.conf }}%</span>
            <span v-if="STATE_LABEL[c.state]" class="badge" :class="c.state === 'unassigned' ? 'badge-warning' : 'badge-default'">{{ STATE_LABEL[c.state] }}</span>
            <span v-if="run.decisions?.[c.key]?.line" class="badge badge-success">Line</span>
          </li>
        </ul>
      </div>
    </div>
  </template>
  <div v-else class="card">
    <div class="card-body empty-state">Start times every car that crosses, by its number</div>
  </div>

  <div v-if="selected" class="card">
    <div class="card-header"><h3>🔎 Photo Finish</h3></div>
    <div class="card-body">
      <PhotoFinishViewer :crossing="selected" :run="run" :ref-tick="refTick" :editable="!!run" :show-car="true" @decide="decide" />
      <div v-if="mode === 'camera-laps'" class="assign">
        <span>Assign to</span>
        <button v-for="car in cars" :key="car" class="btn btn-sm" :class="selected.car === car ? 'btn-primary' : 'btn-ghost'"
          @click="decide({ car, ignore: false })">{{ car }}</button>
        <button class="btn btn-ghost btn-sm" :disabled="selected.state === 'unassigned'" @click="decide({ car: null, ignore: false })">Unassigned</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.sensors { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 1rem; }
.status-line { display: flex; align-items: center; gap: 0.75rem; }
.result { font-size: 1.25rem; font-weight: 700; }
.not-saved { color: var(--accent-danger); }
.rec-list { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.25rem; }
.rec-list li { display: flex; align-items: center; gap: 0.75rem; padding: 0.35rem 0.5rem; border-radius: 6px; }
.rec-list li.pick { cursor: pointer; }
.rec-list li.pick:hover { background: var(--bg-hover); }
.rec-list li.active { background: var(--bg-hover); outline: 1px solid var(--accent-primary); }
.rec-list li.unassigned { color: var(--accent-warning); }
.rec-list li.ignored { color: var(--text-tertiary); text-decoration: line-through; }
.feed { max-height: 320px; overflow-y: auto; }
.who, .ocr { color: var(--text-secondary); }
.car { font-weight: 700; min-width: 3ch; }
.car-table { width: 100%; border-collapse: collapse; }
.car-table th, .car-table td { padding: 0.4rem 0.5rem; text-align: left; border-bottom: 1px solid var(--border-color); white-space: nowrap; }
.car-table select { min-width: 5rem; }
.assign { display: flex; flex-wrap: wrap; align-items: center; gap: 0.4rem; margin-top: 0.75rem; }
</style>
