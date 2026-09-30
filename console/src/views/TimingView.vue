<script setup>
import { ref, computed } from "vue";
import { useTimingStore } from "../stores/timing";
import { useSettingsStore } from "../stores/settings";
import { MODE_LABEL } from "../lib/constants";
import { formatDuration } from "../lib/format";
import { calibrationLabel } from "../lib/results";
import { faultWindow } from "../lib/fault-text";
import RunControlCard from "../components/RunControlCard.vue";
import RunSetupCard from "../components/RunSetupCard.vue";
import TimerCard from "../components/TimerCard.vue";

const timing = useTimingStore();
const settings = useSettingsStore();
const note = ref(settings.state.lastNote || "");

// The page follows the open run's mode; with no run, the selected mode.
const mode = computed(() => timing.run?.mode ?? settings.state.mode);
const verification = computed(() => timing.run?.verification ?? null);

const startList = computed(() => timing.crossingRows("start"));
const finishList = computed(() => timing.crossingRows("finish"));

// An invalid laps run shows only the laps confirmed before the loss, and no total.
const laps = computed(() => (verification.value === "invalid" ? timing.lapRows.filter((l) => l.confirmed) : timing.lapRows));
const lapNs = computed(() => laps.value.map((l) => l.ns).filter((ns) => ns != null));
const best = computed(() => (lapNs.value.length ? lapNs.value.reduce((a, b) => (b < a ? b : a)) : null));
const last = computed(() => (lapNs.value.length ? lapNs.value[lapNs.value.length - 1] : null));
const avgText = computed(() => (lapNs.value.length ? formatDuration(lapNs.value.reduce((a, b) => a + b, 0n), BigInt(lapNs.value.length)) : "—"));
const target = computed(() => timing.run?.lapTarget ?? settings.state.lapTarget);

const status = computed(() => {
  const v = verification.value;
  if (!timing.run) return null;
  if (v === "verified") return { cls: "badge-success", text: timing.run.closed ? "official" : "official so far" };
  if (v === "invalid") return { cls: "badge-danger", text: "invalid" };
  if (v === "dnf") return { cls: "badge-default", text: timing.run.dnfReason || "DNF" };
  return { cls: "badge-warning", text: "pending" };
});

function delta(ns) {
  if (best.value == null || ns == null) return "";
  const d = ns - best.value;
  return d === 0n ? "best" : `+${formatDuration(d)}`;
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
          <div class="card-header"><h3>🟢 Start sensor</h3></div>
          <div class="card-body">
            <div v-if="!startList.length" class="empty-state">No crossing yet</div>
            <ul v-else class="rec-list">
              <li v-for="c in startList" :key="c.key" :class="{ confirmed: c.confirmed }">
                <span class="mono">{{ formatDuration(c.ns) }}</span>
                <span class="who mono">{{ c.node_id }}</span>
                <span class="chk" :title="c.confirmed ? 'Confirmed' : 'Awaiting confirmation'">{{ c.confirmed ? "✓" : "…" }}</span>
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
                <span class="mono">{{ c.ns != null ? `+${formatDuration(c.ns)}` : "—" }}</span>
                <span class="who mono">{{ c.node_id }}</span>
                <span class="chk" :title="c.confirmed ? 'Confirmed' : 'Awaiting confirmation'">{{ c.confirmed ? "✓" : "…" }}</span>
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
            <div class="k">Total{{ verification === "verified" && timing.run?.closed ? " (official)" : "" }}</div>
            <div class="v mono">{{ formatDuration(timing.resultNs) }}</div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><h3>🔄 Lap times<span v-if="verification === 'invalid' && laps.length" class="sub"> — confirmed laps before the loss</span></h3></div>
          <div class="card-body">
            <div v-if="!laps.length" class="empty-state">{{ startList.length ? "First crossing recorded — waiting for lap 1" : "No crossing yet" }}</div>
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
                  <tr v-for="(lap, i) in [...laps].reverse()" :key="laps.length - i" :class="{ confirmed: lap.confirmed, best: lap.ns != null && lap.ns === best }">
                    <td class="idx">{{ laps.length - i }}</td>
                    <td class="mono">{{ formatDuration(lap.ns) }}</td>
                    <td class="mono d">{{ delta(lap.ns) }}</td>
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
          <div class="result-time mono">{{ formatDuration(timing.resultNs) }}</div>
          <div class="result-meta">
            <span v-if="status" class="badge" :class="status.cls">{{ status.text }}</span>
            <span v-if="timing.run && timing.calibrationMethod" class="badge" :class="timing.calibrationMethod === 'nominal' ? 'badge-default' : 'badge-primary'">{{ calibrationLabel(timing.calibrationMethod) }}</span>
            <span v-if="timing.run?.note" class="lbl">{{ timing.run.note }}</span>
            <span v-if="timing.run && timing.run.durable === false" class="lbl bad">not stored durably</span>
          </div>
          <ul v-if="timing.run?.fault?.reasons?.length" class="fault-list">
            <li v-for="(r, i) in timing.run.fault.reasons" :key="i">
              {{ r.reason }}<span v-if="faultWindow(r, timing.run.boundaryTick)" class="win"> ({{ faultWindow(r, timing.run.boundaryTick) }})</span>
            </li>
          </ul>
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
.sub {
  font-size: 0.8rem;
  font-weight: 400;
  color: var(--text-tertiary);
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
.lbl.bad {
  color: var(--accent-danger);
}
.fault-list {
  flex-basis: 100%;
  margin: 0;
  padding-left: 1.2rem;
  font-size: 0.85rem;
  color: var(--accent-danger);
}
.fault-list .win {
  color: var(--text-tertiary);
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
