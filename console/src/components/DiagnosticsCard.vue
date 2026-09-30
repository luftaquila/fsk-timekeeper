<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useDeviceStore } from "../stores/device";
import { fmtNum, fmtAgeMs, fmtPpm } from "../lib/format";
import { telemetryAgeMs, telemetryFresh } from "../lib/quality";
import { errorNames, resetNames } from "../lib/protocol";

const device = useDeviceStore();
const now = ref(Date.now());
let timer = null;
onMounted(() => {
  timer = setInterval(() => (now.value = Date.now()), 1000);
});
onUnmounted(() => clearInterval(timer));

// Master first, then sensors by id.
const rows = computed(() =>
  Object.values(device.telemetry).sort((a, b) => {
    const am = a.node_id === "0";
    const bm = b.node_id === "0";
    if (am !== bm) return am ? -1 : 1;
    return a.node_id.localeCompare(b.node_id);
  }),
);

const isMaster = (r) => r.node_id === "0";
const nodeLabel = (r) => (isMaster(r) ? "Master" : r.node_id);

// The firmware decides the link state; an old report is no report.
function linkState(r) {
  if (!telemetryFresh(r, now.value)) return "none";
  return r.link_state || "lost";
}
const stateLabel = { online: "Online", degraded: "Stale", lost: "Lost", none: "No report" };

function fmtTemp(v) {
  return v == null ? "—" : `${(v / 10).toFixed(1)} °C`;
}
function fmtVolt(mv) {
  return mv == null ? "—" : `${(mv / 1000).toFixed(3)} V`;
}
function clockDrift(r) {
  if (isMaster(r)) {
    const p = device.pps;
    return p?.valid === 1 ? `${fmtPpm(p.ppb)} (GPS, ${p.span} s)` : "no GPS";
  }
  return `${fmtNum(r.skew_ppm)} ppm`;
}
function gpsCell(r) {
  if (!isMaster(r)) return "—";
  const p = device.pps;
  if (!p) return "—";
  return p.fix ? `${p.sats} sats` : "no fix";
}
function timingHealth(r) {
  if (r.provisioned !== 1) return { state: "bad", label: "No key" };
  if (r.clock_source !== "xtal") return { state: "bad", label: "RC clock" };
  if (isMaster(r)) {
    if (r.queue_overflow) return { state: "warn", label: `Queue backpressure ×${r.queue_overflow}` };
    return { state: "good", label: `OK · queue ${r.queue_depth ?? 0}` };
  }
  if (r.capture_overflow || r.fifo_drop) return { state: "bad", label: "Capture/FIFO loss" };
  if (r.sync_valid !== 1 || r.skew_valid !== 1) return { state: "bad", label: "Sync invalid" };
  return { state: "good", label: `OK · sync ${r.sync_age_ms ?? "-"} ms` };
}
function errorsCell(r) {
  const names = errorNames(r.err_flags);
  const extra = [];
  if (isMaster(r) && r.ver_drop) extra.push(`ver_drop ${r.ver_drop}`);
  if (isMaster(r) && r.tx_drop) extra.push(`tx_drop ${r.tx_drop}`);
  return [...names, ...extra].join(", ") || "—";
}
function resetCell(r) {
  return Number.isInteger(r.reset_reason) ? resetNames(r.reset_reason).join(", ") : "—";
}
// Rough Li-ion SoC: 3.3 V → 0 %, 4.2 V → 100 %.
function socPct(mv) {
  return Math.max(0, Math.min(100, Math.round(((mv - 3300) / 900) * 100)));
}
function battTag(r) {
  if (r.batt_mv == null) return "";
  return isMaster(r) ? "USB" : `${socPct(r.batt_mv)}%`;
}
</script>

<template>
  <div class="card">
    <div class="card-header">
      <h3>
        📶 Diagnostics
        <span v-if="device.dropped.count" class="badge badge-warning" :title="JSON.stringify(device.dropped.reasons)">{{ device.dropped.count }} dropped lines</span>
      </h3>
    </div>
    <div class="card-body">
      <div v-if="!rows.length" class="empty-state">No status reports received.</div>
      <div v-else class="table-scroll">
        <table class="diag-table">
          <thead>
            <tr>
              <th>Node</th>
              <th>Link</th>
              <th class="tip" title="Key, HFXO, sync, capture and queue health combined">Timing</th>
              <th class="tip" title="Errors since boot (err_flags); master: radio packets of another version (ver_drop), dropped USB lines (tx_drop)">Errors</th>
              <th class="tip" title="Why the board last reset">Reset</th>
              <th class="tip" title="Signal strength measured by the master (dBm)">RSSI</th>
              <th class="tip" title="Signal-to-noise ratio (dB)">SNR</th>
              <th class="tip" title="Sensor: skew vs master. Master: HFXO vs GPS PPS">Drift</th>
              <th class="tip" title="Master GNSS fix and satellites in use">GPS</th>
              <th class="tip" title="Beacons missed since boot (current consecutive gap)">Missed</th>
              <th class="tip" title="Event delivery latency (ms)">Latency</th>
              <th class="tip" title="nRF die temperature">Temp</th>
              <th class="tip batt" title="Sensor: cell estimate. Master: charge rail">Battery</th>
              <th class="tip" title="Time since the master last heard this node">Heard</th>
              <th class="tip" title="Time since the console received this report">Report</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="r in rows" :key="r.node_id" :class="{ master: isMaster(r) }">
              <td class="mono node">{{ nodeLabel(r) }}</td>
              <td><span class="lbadge" :class="linkState(r)">{{ stateLabel[linkState(r)] }}</span></td>
              <td><span class="health" :class="timingHealth(r).state">{{ timingHealth(r).label }}</span></td>
              <td class="errs">{{ errorsCell(r) }}</td>
              <td class="errs">{{ resetCell(r) }}</td>
              <td class="mono">{{ isMaster(r) ? "—" : `${fmtNum(r.rssi)} dBm` }}</td>
              <td class="mono">{{ isMaster(r) ? "—" : `${fmtNum(r.snr)} dB` }}</td>
              <td class="mono">{{ clockDrift(r) }}</td>
              <td class="mono">{{ gpsCell(r) }}</td>
              <td class="mono">
                <template v-if="isMaster(r)">—</template>
                <template v-else>{{ r.rx_miss ?? 0 }}<span :class="{ gap: r.beacon_gap }"> ({{ r.beacon_gap ?? 0 }})</span></template>
              </td>
              <td class="mono">{{ isMaster(r) ? "—" : `${fmtNum(r.latency_ms, 0)} ms` }}</td>
              <td class="mono">{{ fmtTemp(r.temp_c10) }}</td>
              <td class="mono batt">
                <template v-if="r.batt_mv == null">—</template>
                <template v-else>{{ fmtVolt(r.batt_mv) }}<span class="batt-tag">{{ battTag(r) }}</span></template>
              </td>
              <td class="mono">{{ isMaster(r) || r.last_seen_at == null ? "—" : fmtAgeMs(Math.max(0, now - r.last_seen_at)) }}</td>
              <td class="mono">{{ fmtAgeMs(telemetryAgeMs(r, now)) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
.diag-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.85rem;
}
.diag-table th,
.diag-table td {
  padding: 0.55rem 0.8rem;
  text-align: left;
  border-bottom: 1px solid var(--border-color);
  white-space: nowrap;
}
.diag-table th:not(.batt),
.diag-table td:not(.batt) {
  width: 1%;
}
.diag-table th.batt,
.diag-table td.batt {
  width: 100%;
}
.diag-table th {
  color: var(--text-tertiary);
  font-weight: 600;
}
.diag-table th.tip {
  cursor: help;
  text-decoration: underline dotted;
  text-underline-offset: 3px;
}
.diag-table tbody tr:last-child td {
  border-bottom: none;
}
.diag-table td.node {
  font-weight: 700;
}
.diag-table tr.master td {
  background: var(--bg-secondary);
}
.gap {
  color: var(--accent-warning);
}
.batt-tag {
  margin-left: 0.45rem;
  color: var(--text-tertiary);
}
.lbadge {
  padding: 0.15rem 0.6rem;
  border-radius: 6px;
  font-size: 0.75rem;
  font-weight: 600;
  white-space: nowrap;
}
.lbadge.online {
  background: rgba(16, 185, 129, 0.18);
  color: var(--accent-success);
}
.lbadge.degraded {
  background: rgba(245, 158, 11, 0.18);
  color: var(--accent-warning);
}
.lbadge.lost,
.lbadge.none {
  background: rgba(239, 68, 68, 0.18);
  color: var(--accent-danger);
}
.errs {
  font-size: 0.75rem;
  color: var(--text-secondary);
  white-space: normal;
  min-width: 6rem;
}
.health.good {
  color: var(--accent-success);
}
.health.warn {
  color: var(--accent-warning);
}
.health.bad {
  color: var(--accent-danger);
  font-weight: 700;
}
</style>
