#ifndef MEAS_H
#define MEAS_H

#include <stdint.h>

/* Cached housekeeping. Refresh at most once a second, outside TX deadlines.
 * A failed peripheral is counted and its cached reading becomes 0. */
void meas_service(uint32_t now_ms);

/* Cached die temperature in 0.1 C (a few C above ambient). */
int16_t meas_temp_c10(void);

/* VDDH rail in mV via the SAADC VDDHDIV5 input: the cell minus the diode drop on
 * a battery node, the charge rail on USB. Cached outside TX. */
uint16_t meas_vddh_mv(void);

#endif /* MEAS_H */
