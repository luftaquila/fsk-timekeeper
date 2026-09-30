/* Hang check for RadioLib's BUSY loops (radio_hal.cpp). RadioLib calls yield()
 * on every turn of a loop that polls the radio's BUSY line, and two of those
 * loops have no timeout (the calibration wait in begin(), the PA ramp wait after
 * SetTx). BUSY staying high for a whole limit with no SPI transfer and within
 * one radio call means the radio hung. Pure logic, host-tested. */
#ifndef HANG_GUARD_H
#define HANG_GUARD_H

#include <stdint.h>

typedef struct {
    int      timing; /* BUSY has been high since `since` */
    uint32_t since;
} hang_guard_t;

/* A new radio call or an SPI transfer: the radio made progress. */
static inline void hang_guard_reset(hang_guard_t *g)
{
    g->timing = 0;
}

/* One poll from a BUSY loop; 1 once BUSY has been high for limit_ms. */
static inline int hang_guard_poll(hang_guard_t *g, int busy, uint32_t now_ms, uint32_t limit_ms)
{
    if (!busy) {
        g->timing = 0;
        return 0;
    }
    if (!g->timing) {
        g->timing = 1;
        g->since = now_ms;
        return 0;
    }
    return (uint32_t)(now_ms - g->since) >= limit_ms;
}

#endif /* HANG_GUARD_H */
