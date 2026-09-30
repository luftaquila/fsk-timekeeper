#ifndef MEAS_H
#define MEAS_H

#include <stdint.h>

/* On-die housekeeping measurements (no external parts). One-shot, bounded
 * waits; a peripheral that never finishes is counted and reads 0. */

/* Die temperature in 0.1 C (a few C above ambient). ~36 us. */
int16_t meas_temp_c10(void);

/* VDDH rail in mV via the SAADC VDDHDIV5 input: the cell minus the diode drop on
 * a battery node, the charge rail on USB. ~tens of us; sags under TX. */
uint16_t meas_vddh_mv(void);

#endif /* MEAS_H */
