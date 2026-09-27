#ifndef MEAS_H
#define MEAS_H

#include <stdint.h>

/* On-die housekeeping measurements — no external parts (this board has no usable
 * battery divider; it runs in high-voltage mode with the cell on VDDH). */

/* nRF52840 die temperature in deci-degrees Celsius (e.g. 235 = 23.5 C).
 * Die temp reads a few C above ambient. One-shot, blocking (~few us). */
int16_t meas_temp_c10(void);

/* Measured VDDH supply rail in millivolts, via the SAADC internal VDDHDIV5
 * input (= VDDH/5). On a battery-only node VDDH ~= cell voltage minus the W5
 * Schottky drop; on a USB-powered node it is the charge rail. One-shot,
 * blocking (~tens of us). Sample close to peak load — VDDH sags under TX. */
uint16_t meas_vddh_mv(void);

#endif /* MEAS_H */
