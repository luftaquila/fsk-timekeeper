import { fmtDateTime } from "./format";
import { RADIO_RECOVERY_REASONS, RADIO_RECOVERY_RESULTS, RADIO_HISTORY_LABELS } from "./protocol";

export const LOG_TYPES = { X: "Error", boot: "Boot", console: "Console", radio: "Radio" };
const FAULTS = RADIO_HISTORY_LABELS;
const CONSOLE_LINES = {
  unknown_line: "Unrecognized USB line.", tel_format: "Invalid status report.",
  pps: "Invalid GPS/PPS report.", event_unreadable: "Unreadable event.",
  event_crc: "Event checksum mismatch.", event_boot: "Invalid event boot ID.",
  unreadable_event: "Repeated unreadable event.",
};
const ERRORS = {
  badcmd: "Command rejected.", noprov: "Radio key not set.", keyfail: "Could not save radio key.",
  cfgfail: "Could not save radio settings.", cfgapply: "Settings saved\nRadio start failed; automatic retry pending",
  cfgpending: "Settings saved\nApply pending", clock: "Clock request rejected.",
};

export function logCode(code) {
  return String(code ?? "unknown").toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "") || "unknown";
}

const total = (n) => `${n}${n === 65535 ? "+" : ""}`;
function recoveryLine(r) {
  const reason = RADIO_RECOVERY_REASONS[r.reason];
  const result = RADIO_RECOVERY_RESULTS[r.result];
  return `Resets: ${total(r.count)}\nFailed: ${total(r.failures)}\nLast Reason: ${reason}\nLast Result: ${result}`;
}

function logLine(e, code) {
  if (e.kind === "boot") return `Reset: ${code.replaceAll("_", " ")}.`;
  if (code === "radio_history" && e.historySnapshot) {
    const { faults, recovery } = e.historySnapshot;
    const parts = faults.flatMap((n, i) => n ? [`${FAULTS[i]}: ${total(n)}`] : []);
    if (recovery.count) parts.push(recoveryLine(recovery));
    return `Since Boot\n${parts.join("\n")}\nEvent Time: Unknown`;
  }
  if (code === "radio_recovery" && e.radioSnapshot) return `Since Boot\n${recoveryLine(e.radioSnapshot)}`;
  if (code === "radio_fault" || code === "radio_restored") {
    return String(e.text ?? "").replace(/^(?:Master|Sensor \S+): /, "").replace(/, /g, "\n");
  }
  if (e.kind === "console") {
    if (CONSOLE_LINES[code]) return CONSOLE_LINES[code];
    if (code === "quarantine") return `Event quarantined: ${String(e.text).split(": ")[0]}.`;
  }
  if (e.kind === "X") {
    if (ERRORS[code]) return ERRORS[code];
    const count = /^X \S+ (\d+)$/.exec(String(e.text).trim());
    if (count && code === "queue_full") return `Queue Full Deferrals: ${count[1]} Since Boot`;
    if (count) return `Count: ${count[1]} since boot.`;
    return String(e.text ?? "").replace(/^X \S+\s*/, "") || `${code.replaceAll("_", " ")}.`;
  }
  return e.text ?? "";
}

function boardId(id) {
  if (!/^[0-9a-f]{16}$/i.test(id ?? "")) return id || "-";
  const low = id.slice(8).toUpperCase();
  const high = id.slice(0, 8).toUpperCase();
  return low !== "00000000" ? low : high !== "00000000" ? high : "00000001";
}

// One representation for both the table and JSON export.
export function logRecord(entry) {
  const code = logCode(entry.code);
  return { time: fmtDateTime(entry.at), type: LOG_TYPES[entry.kind] ?? entry.kind, code,
    board: boardId(entry.devid), line: logLine(entry, code) };
}
