/* GPS (ATGM336H breakout) on the master: NMEA over UARTE0, 1PPS captured on
 * TIMER1 (capture.c), HFXO error estimated against PPS and reported on the USB
 * `P` line (proto_usb.h). Ticks on the wire stay raw — the console applies the
 * correction. Master role only; sensors never call in here.
 */
#ifndef GPS_H
#define GPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t pps_tick;  /* TIMER1 tick of the latest PPS edge (0 = none yet) */
    uint32_t utc_s;     /* UTC of that edge, Unix seconds (0 = unknown / no fix) */
    int32_t  ppb;       /* HFXO error, parts per billion, + = timebase fast (0 when invalid) */
    uint8_t  pps_valid; /* 1 = gated window + fresh well-formed RMC A + HFXO; NOT a PPS accuracy certificate */
    uint8_t  fix;       /* GGA fix quality (0 = none) */
    uint8_t  sats;      /* GGA satellites used */
    uint8_t  span_s;    /* seconds in the estimation window (0 when invalid) */
} gps_report_t;

/* Bring up UARTE0 on PIN_GPS_RXD/TXD (9600 8N1), start reception, ask the module
 * for GGA+RMC only. Call once after capture_pps_enable() in the master role. */
void gps_init(void);

/* Drain received NMEA, take PPS edges from the capture, keep the estimator and
 * the PPS<->UTC association current. Call every main-loop pass (not from ISRs). */
void gps_poll(void);

/* A P line is due: a new PPS<->UTC association, or 1 s since the last report. */
int gps_report_due(void);

/* Fill the current report and clear the due flag. */
void gps_report(gps_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H */
