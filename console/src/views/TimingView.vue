<script setup>
import { computed } from "vue";
import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
import { MODE_LABEL } from "../lib/constants";
import { formatDuration, formatDelta } from "../lib/format";
import FaultReasons from "../components/FaultReasons.vue";
import RunControlCard from "../components/RunControlCard.vue";
import RunSetupCard from "../components/RunSetupCard.vue";
import TimerCard from "../components/TimerCard.vue";

const timing = useTimingStore();
const settings = useSettingsStore();
const note = computed({
  get: () => settings.state.lastNote,
  set: (value) => settings.rememberNote(value),
});

// The page follows the current run's mode, open or closed; with no run, the selected mode.
const mode = computed(() => timing.run?.mode ?? settings.state.mode);
const verification = computed(() => timing.run?.verification ?? null);

const startList = computed(() => timing.crossingRows("start"));
const finishList = computed(() => timing.crossingRows("finish"));

// An invalid laps run shows only the laps confirmed before the loss, and no total.
const laps = computed(() => (verification.value === "invalid" ? timing.lapRows.filter((l) => l.confirmed) : timing.lapRows));
const lapNs = computed(() => laps.value.map((l) => l.ns).filter((ns) => ns != null));
const best = computed(() => (lapNs.value.length ? lapNs.value.reduce((a, b) => (b < a ? b : a)) : null));
const last = computed(() => (lapNs.value.length ? lapNs.value[lapNs.value.length - 1] : null));
const avgText = computed(() => (lapNs.value.length ? formatDuration(lapNs.value.reduce((a, b) => a + b, 0n), BigInt(lapNs.value.length)) : "-"));
const target = computed(() => timing.run?.lapTarget ?? settings.state.lapTarget);

const status = computed(() => {
  const v = verification.value;
  if (!timing.run) return null;
  if (timing.run.classification) return {
    cls: timing.run.classification === "DSQ" ? "badge-danger" : "badge-warning",
    text: timing.run.classification,
  };
  if (v === "verified") return null;
  if (v === "invalid") return { cls: "badge-danger", text: "Invalid" };
  if (timing.run.closed) return { cls: "badge-default", text: "Stopped" };
  return { cls: "badge-warning", text: "Pending" };
});

function delta(ns) {
  if (best.value == null || ns == null) return "";
  const d = ns - best.value;
  return d === 0n ? "BEST" : formatDelta(d);
}
function beforeDifference(index) {
  const current = laps.value[index]?.ns;
  const previous = laps.value[index - 1]?.ns;
  return current == null || previous == null ? null : current - previous;
}
function deltaBefore(index) {
  return formatDelta(beforeDifference(index));
}
</script>

<template>
  <div class="page-layout">
    <aside class="sidebar">
      <RunSetupCard v-model="note" />
      <RunControlCard :note="note" />
    </aside>

    <section class="content">
      <TimerCard :title="MODE_LABEL[mode]" :note="timing.armed ? timing.run.note : note" />

      <div v-if="mode === 'sprint'" class="sensors">
        <div class="card">
          <div class="card-header"><h3>🟢 Start</h3></div>
          <div class="card-body">
            <div v-if="!startList.length" class="empty-state">No crossing yet</div>
            <ul v-else class="rec-list">
              <li v-for="c in startList" :key="c.key" :class="{ confirmed: c.confirmed }">
                <span class="crossing-time mono">{{ formatDuration(c.ns) }}</span>
                <span class="who mono">{{ c.node_id }}</span>
              </li>
            </ul>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><h3>🏁 Finish</h3></div>
          <div class="card-body">
            <div v-if="!finishList.length" class="empty-state">No crossing yet</div>
            <ul v-else class="rec-list">
              <li v-for="c in finishList" :key="c.key" :class="{ confirmed: c.confirmed }">
                <span class="crossing-time mono">{{ formatDuration(c.ns) }}</span>
                <span class="who mono">{{ c.node_id }}</span>
              </li>
            </ul>
          </div>
        </div>
      </div>

      <template v-else>
        <div class="stats">
          <div class="stat card">
            <div class="k">Laps</div>
            <div class="v mono">{{ laps.length }}<span v-if="target" class="of">/ {{ target }}</span></div>
          </div>
          <div class="stat card">
            <div class="k">Best</div>
            <div class="v mono">{{ formatDuration(best) }}</div>
          </div>
          <div class="stat card">
            <div class="k">Last</div>
            <div class="v mono">{{ formatDuration(last) }}</div>
          </div>
          <div class="stat card">
            <div class="k">Average</div>
            <div class="v mono">{{ avgText }}</div>
          </div>
          <div class="stat card total">
            <div class="k">Total</div>
            <div class="v mono">{{ formatDuration(timing.resultNs) }}</div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><h3>🔄 Lap Times</h3></div>
          <div class="card-body">
            <p v-if="verification === 'invalid' && laps.length" class="sub">Confirmed laps before data loss</p>
            <div v-if="!laps.length" class="empty-state">{{ startList.length ? "Waiting for First Lap" : "No crossing yet" }}</div>
            <div v-else class="table-scroll">
              <table class="lap-table">
                <colgroup>
                  <col class="lap-column" />
                  <col class="time-column" />
                  <col class="delta-column" />
                  <col class="delta-column" />
                </colgroup>
                <thead>
                  <tr>
                    <th>Lap</th>
                    <th>Time</th>
                    <th>Δ Before</th>
                    <th>Δ Best</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(lap, i) in [...laps].reverse()" :key="laps.length - i" :class="{ confirmed: lap.confirmed, best: lap.ns != null && lap.ns === best }">
                    <td class="idx">{{ laps.length - i }}</td>
                    <td class="mono">{{ formatDuration(lap.ns) }}</td>
                    <td class="mono d" :class="{ slower: beforeDifference(laps.length - i - 1) > 0n }">{{ deltaBefore(laps.length - i - 1) }}</td>
                    <td class="mono d">{{ delta(lap.ns) }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </template>

      <div class="card">
        <div class="card-header"><h3>🏆 Result</h3></div>
        <div class="card-body result-body">
          <div class="result-time mono">{{ formatDuration(timing.resultNs) }}</div>
          <div v-if="status || timing.run?.durable === false" class="result-meta">
            <span v-if="status" class="badge" :class="status.cls">{{ status.text }}</span>
            <span v-if="timing.run && timing.run.durable === false" class="lbl bad">Not Saved</span>
          </div>
          <FaultReasons v-if="timing.run?.fault?.reasons?.length" class="fault-list" :reasons="timing.run.fault.reasons" :boundary-tick="timing.run.boundaryTick" />
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.sensors {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  align-items: start;
  gap: 1.5rem;
}
.rec-list {
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 0.35rem;
}
.rec-list li {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: 0.25rem 0.75rem;
  padding: 0.4rem 0.6rem;
  border-radius: 6px;
  background: var(--bg-secondary);
  font-size: 1.5rem;
  line-height: 1.4;
  color: var(--text-secondary);
}
.rec-list li.confirmed {
  color: var(--text-primary);
}
.crossing-time {
  font-weight: 400;
  white-space: nowrap;
}
.who {
  margin-left: auto;
  font-size: 1.125rem;
  color: var(--text-secondary);
}
.stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(10.5rem, 1fr));
  gap: 1rem;
}
.stat {
  padding: 0;
}
.stat .k {
  font-size: 1.125rem;
  font-weight: 600;
  color: var(--text-primary);
  padding: 0.65rem 1rem;
  background: var(--bg-hover);
  border-bottom: 1px solid var(--border-color);
}
.stat .v {
  padding: 0.8rem 1rem;
  font-size: 1.5rem;
  font-weight: 700;
}
.stat.total {
  border-color: rgba(59, 130, 246, 0.5);
}
.of {
  font-size: 1rem;
  color: var(--text-secondary);
  margin-left: 0.3rem;
}
.sub {
  font-size: 1rem;
  font-weight: 400;
  color: var(--text-secondary);
}
.lap-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 1.125rem;
}
.lap-column { width: 12%; }
.time-column { width: 28%; }
.delta-column { width: 30%; }
.lap-table th {
  text-align: left;
  font-size: 1rem;
  font-weight: 600;
  color: var(--text-secondary);
  padding: 0.6rem 0.75rem;
}
.lap-table td {
  padding: 0.6rem 0.75rem;
  border-bottom: 1px solid var(--border-color);
  color: var(--text-secondary);
}
.lap-table tr:last-child td {
  border-bottom: none;
}
.lap-table tr.confirmed td {
  color: var(--text-primary);
}
.lap-table tr.best td.mono {
  color: var(--accent-success);
  font-weight: 700;
}
.lap-table tr td.d.slower {
  color: var(--accent-danger);
  font-weight: 600;
}
.idx {
  color: var(--text-secondary);
}
.lap-table th:first-child {
  text-align: left;
}
.lap-table td.idx {
  text-align: left;
}
.d {
  color: var(--text-secondary);
}
.result-body {
  display: flex;
  align-items: center;
  gap: 1.5rem;
  flex-wrap: wrap;
  padding: 1.25rem 2rem;
}
.result-time {
  font-size: 2.5rem;
  font-weight: 700;
}
.result-meta {
  display: flex;
  flex-direction: column;
  gap: 0.35rem;
  align-items: flex-start;
}
.lbl {
  font-size: 1rem;
  color: var(--text-secondary);
}
.lbl.bad {
  color: var(--accent-danger);
}
.fault-list {
  flex-basis: 100%;
  margin: 0;
  padding-left: 1.2rem;
  font-size: 1rem;
  color: var(--accent-danger);
}
@media (max-width: 900px) {
  .stats {
    grid-template-columns: repeat(2, 1fr);
  }
  .stat.total {
    grid-column: span 2;
  }
}
@media (max-width: 640px) {
  .sensors {
    grid-template-columns: 1fr;
  }
  .result-time {
    font-size: 1.75rem;
  }
}
</style>
