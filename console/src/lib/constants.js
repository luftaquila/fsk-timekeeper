// Timing modes and instrument-wide constants. Pure module (no Vue).

// Modes (named by mechanism): sprint = first start crossing to the first finish crossing after it;
// laps = start-role crossings only, every crossing after the first ends a lap; camera-sprint =
// sprint with a sensor at the start and the line-scan camera at the finish; camera-laps = laps of
// every car crossing the camera, told apart by their numbers.
export const MODES = ["sprint", "laps", "camera-sprint", "camera-laps"];
export const CAMERA_MODES = ["camera-sprint", "camera-laps"];
export const RESULT_CLASSIFICATIONS = ["DNS", "DNF", "DSQ"];

export const MODE_LABEL = {
  sprint: "Start → Finish",
  laps: "Laps",
  "camera-sprint": "Photo Sprint",
  "camera-laps": "Photo Laps",
};
// The timing rules a mode follows.
export const MODE_KIND = { sprint: "sprint", laps: "laps", "camera-sprint": "sprint", "camera-laps": "laps" };

export const ROLES = ["unused", "start", "finish"];
export const ROLE_LABEL = { unused: "Unused", start: "Start", finish: "Finish" };

// Roles a mode needs mapped before it can be armed; other roles are ignored by that mode.
export const REQUIRED_ROLES = Object.freeze({
  sprint: ["start", "finish"],
  laps: ["start"],
  "camera-sprint": ["start", "finish"],
  "camera-laps": ["start"],
});

// Camera laps: an OCR reading below this confidence leaves a crossing unassigned (operator).
export const DEFAULT_OCR_MIN_CONF = 80;
// Car numbers on the stickers: one to three digits, kept as text without leading zeros.
export const CAR_NUMBER_PATTERN = /^[1-9]\d{0,2}$/;

// Freshness limit of master lines, sensor D reports, admission WAIT and the START checkpoint (not
// link state); the master re-reports registered sensors every 3 s, LOST ones too.
export const WIRELESS_STATUS_MAX_AGE_MS = 12000;
export const WIRELESS_MAX_SKEW_PPM = 100;
export const WIRELESS_SYNC_TTL_MS = 7000;

// START refuses above this master queue fill (depth > 12 of 16), or while one E line repeats for
// over HEAD_STUCK_MS.
export const QUEUE_HEALTH_RATIO = 0.75;
export const HEAD_STUCK_MS = 5000;
// Repeats of one unreadable E line before the fatal alarm.
export const UNREADABLE_ALARM_REPEATS = 10;

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
