// Timing modes and instrument-wide constants. Pure module (no Vue).

// Modes are named by mechanism, not by competition event.
//  - sprint: first start crossing opens the interval, the first finish crossing after it closes it.
//  - laps:   start-role crossings only; every crossing after the first ends a lap.
export const MODES = ["sprint", "laps"];

export const MODE_LABEL = {
  sprint: "Start → Finish",
  laps: "Laps",
};

export const ROLES = ["start", "finish"];
export const ROLE_LABEL = { start: "Start", finish: "Finish" };

// Roles a mode needs mapped before it can be armed; other roles are ignored by that mode.
export const REQUIRED_ROLES = Object.freeze({
  sprint: ["start", "finish"],
  laps: ["start"],
});

// A D line older than this is no report (= the firmware's LINK_OK_MS).
export const WIRELESS_STATUS_MAX_AGE_MS = 12000;
export const WIRELESS_MAX_SKEW_PPM = 100;

// START refuses while the master's host queue is this full, or while the same E line keeps
// arriving for longer than HEAD_STUCK_MS.
export const QUEUE_HEALTH_RATIO = 0.75;
export const HEAD_STUCK_MS = 5000;
// Repeats of one unreadable E line before the fatal alarm.
export const UNREADABLE_ALARM_REPEATS = 10;

// A decided result waits at most this long for the next qualified PPS edge before freezing.
export const CALIBRATION_WAIT_MS = 2000;

export const LOG_RETENTION_MS = 30 * 24 * 3600 * 1000;
export const LOG_MAX_ENTRIES = 5000;
export const PPS_RETENTION_MS = 7 * 24 * 3600 * 1000;

export const DEFAULT_DEBOUNCE_MS = 300;
export const DEBOUNCE_MIN_MS = 0;
export const DEBOUNCE_MAX_MS = 5000;

export const USB_VID = 0x1999;
export const USB_PID = 0x0515;
export const USB_PRODUCT = "FSK-WL";
export const SERIAL_BAUD = 115200;
