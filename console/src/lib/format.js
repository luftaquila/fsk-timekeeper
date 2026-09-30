// Display formatting. Durations are stored as exact ns; this is the only place they are rounded.

const pad = (v, n) => String(v).padStart(n, "0");

// ns / divisor -> "MM:SS.mmm" (minutes may exceed 59), milliseconds rounded half up once.
// null -> "—". Negative spans show as zero.
export function formatDuration(ns, divisor = 1n) {
  if (ns == null) return "—";
  let n = BigInt(ns);
  if (n < 0n) n = 0n;
  const d = BigInt(divisor) * 1_000_000n;
  const ms = (2n * n + d) / (2n * d);
  return `${pad(ms / 60000n, 2)}:${pad((ms % 60000n) / 1000n, 2)}.${pad(ms % 1000n, 3)}`;
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
