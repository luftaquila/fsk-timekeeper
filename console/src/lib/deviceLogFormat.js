import { RADIO_RECOVERY_REASONS, RADIO_RECOVERY_RESULTS } from "./protocol";

const TYPES = { X: "Error", boot: "Boot", console: "Console", radio: "Radio" };
const FAULTS = ["CAD failures", "SPI timeouts", "TX failures", "RX failures", "PLL/oscillator failures", "init failures"];
const CONSOLE_LINES = {
  unknown_line: "Unrecognized USB line.", tel_format: "Invalid status report.",
  pps: "Invalid GPS/PPS report.", event_unreadable: "Unreadable event.",
  event_crc: "Event checksum mismatch.", event_boot: "Invalid event boot ID.",
  unreadable_event: "Repeated unreadable event.",
};
const ERRORS = {
  badcmd: "Command rejected.", noprov: "Radio key not set.", keyfail: "Could not save radio key.",
  cfgfail: "Could not save radio settings.", cfgapply: "Radio settings could not be applied.",
  cfgpending: "Radio settings saved; recovery pending.", clock: "Clock request rejected.",
};

export function logCode(code) {
  return String(code ?? "unknown").toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "") || "unknown";
}

const total = (n) => `${n}${n === 65535 ? "+" : ""}`;
function recoveryLine(r) {
  const reason = r.reason === 2 ? "no beacon >10 s" : RADIO_RECOVERY_REASONS[r.reason];
  const result = r.result === 2 ? "RX restarted" : RADIO_RECOVERY_RESULTS[r.result];
  return `resets ${total(r.count)}, failed ${total(r.failures)}. Last: ${reason} → ${result}`;
}

function logLine(e, code) {
  if (e.kind === "boot") return `Reset: ${code.replaceAll("_", " ")}.`;
  if (code === "radio_history" && e.historySnapshot) {
    const { faults, recovery } = e.historySnapshot;
    const parts = faults.flatMap((n, i) => n ? [`${FAULTS[i]}: ${total(n)}`] : []);
    if (recovery.count) parts.push(recoveryLine(recovery));
    return `Since boot: ${parts.join("; ")}. Event time unknown.`;
  }
  if (code === "radio_recovery" && e.radioSnapshot) return `Since boot: ${recoveryLine(e.radioSnapshot)}.`;
  if (code === "radio_fault" || code === "radio_restored") {
    return String(e.text ?? "").replace(/^(?:Master|Sensor \S+): /, "");
  }
  if (e.kind === "console") {
    if (CONSOLE_LINES[code]) return CONSOLE_LINES[code];
    if (code === "quarantine") return `Event quarantined: ${String(e.text).split(": ")[0]}.`;
  }
  if (e.kind === "X") {
    if (ERRORS[code]) return ERRORS[code];
    const count = /^X \S+ (\d+)$/.exec(String(e.text).trim());
    if (count) return `Count: ${count[1]} since boot.`;
    return String(e.text ?? "").replace(/^X \S+\s*/, "") || `${code.replaceAll("_", " ")}.`;
  }
  return e.text ?? "";
}

function logTime(ms) {
  const date = new Date(ms);
  if (Number.isNaN(date.getTime())) return "";
  const pad = (n) => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${date.toLocaleTimeString()}`;
}

function boardId(id) {
  if (!/^[0-9a-f]{16}$/i.test(id ?? "")) return id || "—";
  const low = id.slice(8).toUpperCase();
  const high = id.slice(0, 8).toUpperCase();
  return low !== "00000000" ? low : high !== "00000000" ? high : "00000001";
}

// One representation for both the table and JSON export, including older entries.
export function logRecord(entry) {
  const code = logCode(entry.code);
  return { time: logTime(entry.at), type: TYPES[entry.kind] ?? entry.kind, code,
    board: boardId(entry.devid), line: logLine(entry, code) };
}
