// Timing modes and instrument-wide constants. Pure module (no Vue).

// Modes are named by mechanism, not by competition event.
//  - sprint: two sensors, first start crossing opens the interval, first finish crossing closes it.
//  - laps:   one sensor, first crossing = t0, every later crossing ends a lap; result = sum of laps.
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

// Telemetry older than this is treated as unknown (server contract).
export const WIRELESS_STATUS_MAX_AGE_MS = 12000;
// A sensor sync anchor older than this cannot back a normal capture.
export const WIRELESS_SYNC_MAX_AGE_MS = 7000;
export const WIRELESS_MAX_SKEW_PPM = 100;

export const DEFAULT_DEBOUNCE_MS = 300;
export const DEBOUNCE_MIN_MS = 0;
export const DEBOUNCE_MAX_MS = 5000;

export const USB_VID = 0x1999;
export const USB_PID = 0x0515;
export const USB_PRODUCT = "FSK-WL";
export const SERIAL_BAUD = 115200;
