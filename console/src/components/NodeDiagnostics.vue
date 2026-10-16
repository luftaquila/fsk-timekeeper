<script setup>
import { computed, ref, onMounted, onUnmounted } from "vue";
import MetricValue from "./MetricValue.vue";
import { useDeviceStore } from "../stores/device";
import { fmtNum, fmtPpm, fmtAgeMs } from "../lib/format";
import { telemetryFresh, timingHealth } from "../lib/quality";
import { errorNames, ERROR_BITS, MASTER_QUEUE_CAPACITY } from "../lib/protocol";

const props = defineProps({ nodeId: { type: String, required: true } });
const device = useDeviceStore();
const report = computed(() => device.telemetry[props.nodeId]);
const now = ref(Date.now());
const health = computed(() => timingHealth(report.value, {
  nodeId: props.nodeId, connected: device.connected, admission: device.admission?.[props.nodeId], now: now.value,
}));
let timer = null;
onMounted(() => {
  timer = setInterval(() => (now.value = Date.now()), 1000);
});
onUnmounted(() => clearInterval(timer));

const isMaster = (r) => r.node_id === "0";

// A missing report does not change the last firmware-reported radio state.
const reportFresh = computed(() => device.connected && telemetryFresh(report.value, now.value));
const stateLabel = { online: "ONLINE", degraded: "STALE", lost: "LOST" };

const metric = (value, unit = "") => ({ value, unit });
const temperature = (v) => metric(v == null ? "-" : (v / 10).toFixed(1), "°C");
const voltage = (mv) => metric(mv == null ? "-" : (mv / 1000).toFixed(3), "V");
function clockDrift(r) {
  if (isMaster(r)) {
    const p = device.gpsReport(now.value);
    if (!p) return { value: "NO PPS", tone: "warn" };
    if (p.span < 8) return { value: p.fix ? "MEASURING" : "MEASURING (NO FIX)", tone: "warn" };
    if (!p.fix && p.span >= 8) return {
      parts: [metric(fmtPpm(p.ppb).replace(/ ppm$/, ""), "ppm"), "(NO FIX)"],
      tone: "warn",
    };
    if (p.valid === 1 && p.fix) return {
      parts: [metric(fmtPpm(p.ppb).replace(/ ppm$/, ""), "ppm")],
    };
    return { value: "NOT READY", tone: "warn" };
  }
  return { parts: [metric(fmtNum(r.skew_ppm), "ppm")] };
}
const errors = computed(() => {
  const r = report.value;
  let flags = r?.err_flags;
  // Older firmware latched this flag even after ACKs drained the queue.
  if (props.nodeId === "0" && Number.isInteger(r?.queue_depth)) {
    flags = ((flags ?? 0) & ~ERROR_BITS.QUEUE_FULL) |
      (r.queue_depth >= MASTER_QUEUE_CAPACITY ? ERROR_BITS.QUEUE_FULL : 0);
  }
  const items = errorNames(flags).map((label) => ({ label, tone: "badge-danger" }));
  if (props.nodeId !== "0" && r?.diag_valid === 1) {
    if (r.capture_overflow > 0) items.push({ label: `Capture losses (total): ${r.capture_overflow}`, tone: "badge-warning" });
    if (r.fifo_drop > 0) items.push({ label: `FIFO losses (total): ${r.fifo_drop}`, tone: "badge-warning" });
  }
  if (props.nodeId === "0") {
    if (device.identity && !device.contract.ok) items.push({ label: device.contract.label || device.contract.reason, tone: "badge-danger" });
    if (r?.tx_drop > 0) items.push({ label: `USB TX drops: ${r.tx_drop}`, tone: "badge-danger" });
    if (device.dropped.count) items.push({
      label: `${device.dropped.count} dropped ${device.dropped.count === 1 ? "line" : "lines"}`,
      tone: "badge-warning",
    });
  }
  return items;
});
const hasErrors = computed(() => errors.value.length > 0);
// Rough Li-ion SoC: 3.3 V → 0 %, 4.2 V → 100 %.
function socPct(mv) {
  return Math.max(0, Math.min(100, Math.round(((mv - 3300) / 900) * 100)));
}
// SX126x packet status: RSSI = -u8/2 dBm; SNR = signed u8/4 dB.
// Fixed display scales make bars comparable across sensors; this is not a quality percentage.
// SNR focuses on the operating range; only the bar is clamped, never the numeric readout.
function signalGauge(value, min, max, unit, recommendedMin) {
  const known = Number.isFinite(value);
  return { min, max, unit, number: known ? fmtNum(value) : "-",
    // Color the entire segment containing the warning boundary.
    warningPercent: Math.max(0, Math.min(100, Math.ceil((recommendedMin - min) / (max - min) * 10) * 10)),
    value: known ? Math.max(min, Math.min(max, value)) : null,
    percent: known ? Math.max(0, Math.min(100, (value - min) / (max - min) * 100)) : 0 };
}
const overview = computed(() => {
  const r = report.value;
  const h = health.value;
  const parts = h.ok && h.syncAgeMs != null ? ["OK", { ...metric(Math.round(h.syncAgeMs), "ms"), parenthesized: true }] :
    h.ok && props.nodeId === "0" && r?.queue_depth > 0 ? [metric(r.queue_depth, "QUEUED")] : undefined;
  const common = [
    { label: "Link", value: reportFresh.value ? stateLabel[r.link_state] || "UNKNOWN" : "NO REPORT",
      title: reportFresh.value ? undefined : `Last reported link: ${stateLabel[r?.link_state] || "UNKNOWN"}`,
      tone: reportFresh.value && r.link_state === "online" ? "good" : "warn" },
    { label: "Timing", value: h.label, parts, tone: h.state },
  ];
  if (!r) return common;
  if (r.diag_valid !== 1) {
    const labels = isMaster(r) ? ["Clock Drift"] : ["Clock Drift", "RSSI", "SNR", "Latency", "Beacon Lost", "Battery", "Temperature"];
    return [...common, ...labels.map((label) => ({ label, value: "—" }))];
  }
  return isMaster(r) ? [...common,
    { label: "Clock Drift", ...clockDrift(r) },
  ] : [...common,
    { label: "Clock Drift", ...clockDrift(r) },
    { label: "RSSI", value: `${fmtNum(r.rssi)} dBm`, gauge: signalGauge(r.rssi, -130, 0, "dBm", -105) },
    { label: "SNR", value: `${fmtNum(r.snr)} dB`, gauge: signalGauge(r.snr, -10, 20, "dB", -2.5) },
    { label: "Latency", parts: [metric(fmtNum(r.latency_ms, 0), "ms")] },
    { label: "Beacon Lost", parts: [metric(r.rx_miss ?? 0, "Total"), "/", metric(r.beacon_gap ?? 0, "Consecutive")] },
    { label: "Battery", parts: [metric(r.batt_mv == null ? "-" : socPct(r.batt_mv), "%"), { ...voltage(r.batt_mv), parenthesized: true }] },
    { label: "Temperature", parts: [temperature(r.temp_c10)] },
  ];
});
const detail = computed(() => {
  const r = report.value;
  if (!r || r.diag_valid !== 1) return [];
  return isMaster(r) ? [
    { label: "Supply Voltage", parts: [voltage(r.batt_mv), "(USB)"] },
    { label: "Temperature", parts: [temperature(r.temp_c10)] },
  ] : [];
});
const fields = computed(() => {
  if (props.nodeId !== "0") return [...overview.value, ...detail.value];
  const uptime = device.heartbeat ? fmtAgeMs(device.heartbeat.uptimeMs) : "-";
  return [...overview.value,
    { label: "Uptime", parts: [/^\d+[sm]$/.test(uptime) ? metric(uptime.slice(0, -1), uptime.slice(-1)) : metric(uptime)] },
    ...detail.value];
});

</script>

<template>
  <div class="node-diagnostics" :class="{ 'has-errors': hasErrors, 'master-diagnostics': nodeId === '0', 'sensor-diagnostics': nodeId !== '0' }">
    <dl v-if="report || hasErrors || fields.length" class="diagnostic-grid">
      <div v-for="field in fields" :key="field.label">
        <dt>{{ field.label }}</dt>
        <dd :title="field.title" :class="[field.tone, { mono: field.mono, 'signal-value': field.gauge, 'timing-value': field.label === 'Timing' }]">
          <span v-if="field.gauge" class="signal-gauge" :role="field.gauge.value == null ? undefined : 'meter'"
            :aria-label="field.label" :aria-valuemin="field.gauge.min" :aria-valuemax="field.gauge.max"
            :aria-valuenow="field.gauge.value ?? undefined" :aria-valuetext="field.value">
            <span class="signal-track">
              <span class="signal-fill" :style="{ width: `${field.gauge.percent}%` }"></span>
              <span class="signal-warning-range" aria-hidden="true" :style="{ width: `${Math.min(field.gauge.warningPercent, field.gauge.percent)}%` }"></span>
            </span>
          </span>
          <MetricValue v-if="field.gauge" class="signal-readout" :value="field.gauge.number" :unit="field.gauge.unit" />
          <span v-else-if="field.parts" class="metric-parts">
            <template v-for="(part, index) in field.parts" :key="index">
              <MetricValue v-if="typeof part === 'object'" :value="part.value" :unit="part.unit" :parenthesized="part.parenthesized" />
              <span v-else class="metric-context">{{ part }}</span>{{ " " }}
            </template>
          </span>
          <span v-else>{{ field.value }}</span>
        </dd>
      </div>
      <div v-if="hasErrors" class="diagnostic-errors">
        <dt>Errors</dt>
        <dd class="error-chips">
          <span v-for="error in errors" :key="error.label" class="badge" :class="error.tone">{{ error.label }}</span>
        </dd>
      </div>
    </dl>
  </div>
</template>

<style scoped>
.diagnostic-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 280px), 1fr));
  gap: 0.65rem 1.5rem;
  margin: 0;
}
.diagnostic-grid > div { display: grid; grid-template-columns: 7.5rem minmax(0, 1fr); gap: 0.75rem; min-width: 0; min-height: 1.7rem; align-items: center; }
.diagnostic-grid > .diagnostic-errors { grid-column: 1 / -1; align-items: center; }
@container sensor-layout (min-width: 888px) {
  .sensor-diagnostics .diagnostic-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
}
@container master-layout (min-width: 888px) {
  .master-diagnostics .diagnostic-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
}
@container sensor-layout (max-width: 279px) {
  .sensor-diagnostics .diagnostic-grid > div { grid-template-columns: 5.5rem minmax(0, 1fr); }
}
.signal-value { display: flex; align-items: center; gap: 0.75rem; min-width: 0; }
.signal-readout {
  display: inline-flex;
  align-items: baseline;
  column-gap: 0.3rem;
  flex-shrink: 0;
  white-space: nowrap;
  font-family: var(--font-mono);
  font-size: 1.125rem;
  font-variant-numeric: tabular-nums;
}
.metric-parts { display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.3rem; }
.metric-context { color: var(--text-secondary); }
:deep(.parenthesized::before), :deep(.parenthesized::after) { color: var(--text-secondary); }
.good .metric-context, .warn .metric-context, .bad .metric-context { color: inherit; }
.signal-gauge {
  display: block;
  flex: 1 1 5rem;
  min-width: 1.5rem;
  max-width: 5rem;
  height: 0.65rem;
  padding: 2px;
  border: 1px solid color-mix(in srgb, var(--border-color) 65%, transparent);
  border-radius: 3px;
  background: color-mix(in srgb, var(--border-color) 12%, transparent);
  box-sizing: content-box;
  overflow: hidden;
}
.signal-warning-range {
  position: absolute;
  top: 0;
  bottom: 0;
  left: 0;
  background: #eab308;
}
.signal-track {
  display: block;
  position: relative;
  height: 100%;
  background: color-mix(in srgb, var(--border-color) 55%, transparent);
  mask-image: repeating-linear-gradient(to right, #000 0, #000 calc(10% - 2px), transparent calc(10% - 2px), transparent 10%);
}
.signal-fill {
  display: block;
  height: 100%;
  background: linear-gradient(to right, color-mix(in srgb, var(--accent-success) 65%, #22b8a8), var(--accent-success));
}
.error-chips { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.error-chips .badge { max-width: 100%; }
dt { color: var(--text-secondary); font-size: 1rem; }
dd { margin: 0; color: color-mix(in srgb, var(--text-primary) 30%, var(--text-secondary)); font-family: var(--font-mono); font-size: 1.125rem; font-weight: 600; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
dd :deep(.metric-number) { font-size: 1.125rem; }
dd :deep(.metric-unit) { color: var(--text-secondary); }
.good { color: var(--accent-success); }
.warn { color: var(--accent-warning); }
.bad { color: var(--accent-danger); }
.no-report { color: var(--text-secondary); font-size: 1rem; }
</style>
