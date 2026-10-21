#ifndef MEAS_H
#define MEAS_H

#include <stdint.h>

/* Refresh at most 1/s, outside TX deadlines; a failed peripheral is counted and caches 0. */
void meas_service(uint32_t now_ms);

/* Cached die temperature in 0.1 C (a few C above ambient). */
int16_t meas_temp_c10(void);

/* VDDH in mV (SAADC VDDHDIV5): cell minus diode drop on a battery node, charge rail on USB. */
uint16_t meas_vddh_mv(void);

#endif /* MEAS_H */
