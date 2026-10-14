#ifndef RETRY_H
#define RETRY_H

#include <stdint.h>

/* Scheduling only: callers own recovery actions and their timebase lifetime. */
typedef struct { uint32_t at_ms, delay_ms; } retry_t;

static inline void retry_init(retry_t *r, uint32_t now, uint32_t delay)
{
    r->at_ms = now + delay;
    r->delay_ms = delay;
}

static inline int retry_due(const retry_t *r, uint32_t now)
{
    return (int32_t)(now - r->at_ms) >= 0;
}

static inline void retry_later(retry_t *r, uint32_t now, uint32_t limit)
{
    r->at_ms = now + r->delay_ms;
    r->delay_ms = r->delay_ms > limit / 2u ? limit : r->delay_ms * 2u;
}

#endif
