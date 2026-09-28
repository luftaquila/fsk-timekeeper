import { MASTER_TICKS_PER_MS as TICKS_PER_MS } from "./event-timing";

// ms -> "MM:SS.mmm" (minutes may exceed 59). Fractional input is floored.
export function msToClockStr(ms) {
  if (!Number.isFinite(ms) || ms < 0) ms = 0;
  ms = Math.floor(ms);
  const minutes = String(Math.floor(ms / 60000)).padStart(2, "0");
  const seconds = String(Math.floor((ms % 60000) / 1000)).padStart(2, "0");
  const millis = String(ms % 1000).padStart(3, "0");
  return `${minutes}:${seconds}.${millis}`;
}

// Fractional ms between two raw ticks (display only; official results round once via event-timing).
// ppb = master HFXO error from GPS PPS, 0 = nominal.
export function tickDeltaToMs(end, start, ppb = 0) {
  return (Number(BigInt(end) - BigInt(start)) / Number(TICKS_PER_MS)) * (1e9 / (1e9 + ppb));
}

// ppb -> "±x.xx ppm"
export function fmtPpm(ppb) {
  const ppm = ppb / 1000;
  return `${ppm >= 0 ? "+" : "−"}${Math.abs(ppm).toFixed(2)} ppm`;
}

export function fmtNum(v, digits = 1) {
  return v == null || Number.isNaN(v) ? "-" : Number(v).toFixed(digits);
}

export function fmtAgeMs(a) {
  if (!Number.isFinite(a)) return "-";
  const s = Math.floor(a / 1000);
  return s < 60 ? `${s}s` : `${Math.floor(s / 60)}m`;
}

export function fmtDateTime(ms) {
  const d = new Date(ms);
  return Number.isNaN(d.getTime()) ? "" : d.toLocaleString();
}

// RFC 4180 field escaping.
export function csvField(value) {
  const s = value == null ? "" : String(value);
  return /[",\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

export function toCsv(headers, rows) {
  const lines = [headers.map(csvField).join(",")];
  for (const row of rows) lines.push(row.map(csvField).join(","));
  return lines.join("\r\n") + "\r\n";
}

export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export function timestampSlug(ms = Date.now()) {
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}
