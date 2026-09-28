<script setup>
import { ref, computed } from "vue";
import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
import { MODE_LABEL } from "../lib/constants";
import { msToClockStr, fmtPpm } from "../lib/format";
import RunControlCard from "../components/RunControlCard.vue";
import RunSetupCard from "../components/RunSetupCard.vue";
import TimerCard from "../components/TimerCard.vue";

const timing = useTimingStore();
const settings = useSettingsStore();
const source = timing.source;
const note = ref(settings.state.lastNote || "");

// The page follows the open run's mode; with no run, the selected mode.
const mode = computed(() => timing.run?.mode ?? settings.state.mode);

const startList = computed(() => source.crossings.filter((c) => c.role === "start"));
const finishList = computed(() => source.crossings.filter((c) => c.role === "finish"));

const laps = computed(() => source.rawLaps);
const lapMs = computed(() => laps.value.map((l) => l.ms));
const best = computed(() => (lapMs.value.length ? Math.min(...lapMs.value) : null));
const last = computed(() => (lapMs.value.length ? lapMs.value[lapMs.value.length - 1] : null));
const avg = computed(() => (lapMs.value.length ? lapMs.value.reduce((a, b) => a + b, 0) / lapMs.value.length : null));
const total = computed(() => (lapMs.value.length ? lapMs.value.reduce((a, b) => a + b, 0) : null));
const target = computed(() => timing.run?.lapTarget ?? settings.state.lapTarget);

function delta(ms) {
  if (best.value == null) return "";
  const d = ms - best.value;
  return d === 0 ? "best" : `+${(d / 1000).toFixed(3)}`;
}
</script>

<template>
  <div class="page-layout">
    <aside class="sidebar">
      <RunSetupCard v-model="note" />
      <RunControlCard :source="source" :note="note" />
    </aside>

    <section class="content">
      <TimerCard :source="source" :title="MODE_LABEL[mode]" :note="timing.armed ? timing.run.note : note" />

      <div v-if="mode === 'sprint'" class="sensors">
        <div class="card">
          <div class="card-header"><h3>🟢 Start sensor</h3></div>
          <div class="card-body">
            <div v-if="!startList.length" class="empty-state">No crossing yet</div>
            <ul v-else class="rec-list">
              <li v-for="c in startList" :key="c.key" :class="{ confirmed: c.confirmed }">
                <span class="mono">{{ msToClockStr(c.ms) }}</span>
                <span class="who mono">{{ c.node_id }}</span>
                <span class="chk" :title="c.confirmed ? 'Confirmed by checkpoint' : 'Awaiting checkpoint'">{{ c.confirmed ? "✓" : "…" }}</span>
              </li>
            </ul>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><h3>🏁 Finish sensor</h3></div>
          <div class="card-body">
            <div v-if="!finishList.length" class="empty-state">No crossing yet</div>
            <ul v-else class="rec-list">
              <li v-for="c in finishList" :key="c.key" :class="{ confirmed: c.confirmed }">
                <span class="mono">{{ c.ms != null ? `+${msToClockStr(c.ms)}` : "—" }}</span>
                <span class="who mono">{{ c.node_id }}</span>
                <span class="chk" :title="c.confirmed ? 'Confirmed by checkpoint' : 'Awaiting checkpoint'">{{ c.confirmed ? "✓" : "…" }}</span>
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
            <div class="v mono">{{ best != null ? msToClockStr(best) : "—" }}</div>
          </div>
          <div class="stat card">
            <div class="k">Last</div>
            <div class="v mono">{{ last != null ? msToClockStr(last) : "—" }}</div>
          </div>
          <div class="stat card">
            <div class="k">Average</div>
            <div class="v mono">{{ avg != null ? msToClockStr(avg) : "—" }}</div>
          </div>
          <div class="stat card total">
            <div class="k">Total{{ source.verification === "verified" ? " (official)" : "" }}</div>
            <div class="v mono">{{ source.result != null ? msToClockStr(source.result) : total != null ? msToClockStr(total) : "—" }}</div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><h3>🔄 Lap times</h3></div>
          <div class="card-body">
            <div v-if="!laps.length" class="empty-state">{{ source.crossings.length ? "First crossing recorded — waiting for lap 1" : "No crossing yet" }}</div>
            <div v-else class="table-scroll">
              <table class="lap-table">
                <thead>
                  <tr>
                    <th>Lap</th>
                    <th>Time</th>
                    <th>Δ best</th>
                    <th></th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(lap, i) in [...laps].reverse()" :key="laps.length - i" :class="{ confirmed: lap.confirmed, best: lap.ms === best }">
                    <td class="idx">{{ laps.length - i }}</td>
                    <td class="mono">{{ msToClockStr(lap.ms) }}</td>
                    <td class="mono d">{{ delta(lap.ms) }}</td>
                    <td class="chk">{{ lap.confirmed ? "✓" : "…" }}</td>
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
          <div class="result-time mono">{{ source.result != null ? msToClockStr(source.result) : "—" }}</div>
          <div class="result-meta">
            <span v-if="source.verification === 'verified'" class="badge badge-success">official</span>
            <span v-else-if="source.verification === 'invalid'" class="badge badge-danger">invalid</span>
            <span v-else-if="source.run" class="badge badge-warning">pending</span>
            <span v-if="source.run" class="badge" :class="source.calib ? 'badge-primary' : 'badge-default'">{{ source.calib ? `GPS-calibrated ${fmtPpm(source.calib.ppb)}` : "nominal 16 MHz" }}</span>
            <span v-if="source.run?.note" class="lbl">{{ source.run.note }}</span>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.sensors {
  display: grid;
  grid-template-columns: 1fr 1fr;
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
  align-items: center;
  gap: 0.75rem;
  padding: 0.4rem 0.6rem;
  border-radius: 6px;
  background: var(--bg-secondary);
  font-size: 1.05rem;
  color: var(--text-secondary);
}
.rec-list li.confirmed {
  color: var(--text-primary);
}
.who {
  margin-left: auto;
  font-size: 0.75rem;
  color: var(--text-tertiary);
}
.chk {
  width: 1.2rem;
  text-align: center;
  color: var(--accent-success);
}
.stats {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 1rem;
}
.stat {
  padding: 0.9rem 1rem;
}
.stat .k {
  font-size: 0.75rem;
  color: var(--text-tertiary);
  text-transform: uppercase;
  letter-spacing: 0.05em;
}
.stat .v {
  font-size: 1.4rem;
  font-weight: 700;
}
.stat.total {
  border-color: rgba(59, 130, 246, 0.5);
}
.of {
  font-size: 0.9rem;
  color: var(--text-tertiary);
  margin-left: 0.3rem;
}
.lap-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 1rem;
}
.lap-table th {
  text-align: left;
  font-size: 0.75rem;
  color: var(--text-tertiary);
  padding: 0.3rem 0.6rem;
}
.lap-table td {
  padding: 0.4rem 0.6rem;
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
.idx {
  color: var(--text-tertiary);
  width: 4rem;
}
.d {
  color: var(--text-tertiary);
}
.lap-table .chk {
  text-align: right;
  width: auto;
}
.result-body {
  display: flex;
  align-items: center;
  gap: 1.5rem;
  flex-wrap: wrap;
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
  font-size: 0.85rem;
  color: var(--text-secondary);
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
