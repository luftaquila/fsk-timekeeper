/* GPS (ATGM336H breakout) on the master: NMEA over UARTE0, 1PPS captured on
 * TIMER1 (capture.c). Each PPS edge is reported once on the USB `P` line with
 * its qualification segment; ticks stay raw — the console converts them.
 * Master role only, main-loop context.
 */
#ifndef GPS_H
#define GPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t pps_tick;  /* TIMER1 tick of the reported PPS edge (0 = none yet) */
    uint32_t utc_s;     /* UTC of that edge, Unix seconds (0 = unknown) */
    int32_t  ppb;       /* HFXO error, + = timebase fast (0 when invalid) */
    uint8_t  pps_valid; /* 1 = >= PPS_MIN_SPAN_S qualified edges in the window */
    uint8_t  fix;       /* GGA fix quality (0 = none) */
    uint8_t  sats;      /* GGA satellites used */
    uint8_t  span_s;    /* seconds in the estimation window (0 when invalid) */
    uint32_t seg;       /* qualification segment of the edge, 0 = not qualified */
    uint32_t n;         /* index of the edge in its segment (seconds since its first edge) */
} gps_report_t;

/* UARTE0 on PIN_GPS_RXD/TXD (9600 8N1), ask the module for GGA+RMC only.
 * Call once after capture_pps_enable(). */
void gps_init(void);

/* Drain NMEA, take PPS edges, keep qualification and the edge<->UTC
 * association current. Call every main-loop pass. */
void gps_poll(void);

/* A P line is due: a new edge got its RMC (or waited GPS_RMC_LAG_MAX_MS), or
 * 1 s passed since the last report. */
int gps_report_due(void);

/* Fill the report (the newest settled edge) and clear the due state. */
void gps_report(gps_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H */
