#include "cam_track.h"

static uint64_t on_grid(const cam_track_t *c, uint64_t line)
{
    return c->t0 + (line - c->n0) * c->period;
}

void cam_track_init(cam_track_t *c, uint32_t period, uint64_t start_tick)
{
    c->period = period;
    c->start_tick = start_tick;
    c->anchored = 0;
    c->n0 = c->t0 = c->n1 = c->t1 = 0;
    c->broken = 0;
}

void cam_track_anchor(cam_track_t *c, uint64_t n, uint64_t t)
{
    if (c->broken) { return; }
    if (!c->anchored) {
        c->anchored = 1;
        c->n0 = c->n1 = n;
        c->t0 = c->t1 = t;
        return;
    }
    if (n <= c->n1) { return; }
    uint64_t grid = on_grid(c, n);
    if (t + CAM_JITTER_TICKS < grid || t > grid + CAM_JITTER_TICKS) {
        c->broken = 1;
        return;
    }
    c->n1 = n;
    c->t1 = t;
}

uint64_t cam_track_known(const cam_track_t *c)
{
    return c->anchored ? c->n1 : 0;
}

int cam_track_line(const cam_track_t *c, uint64_t line, uint64_t *tick, uint64_t *run_lo, uint64_t *run_hi)
{
    if (line > cam_track_known(c)) { return CAM_LINE_WAIT; }
    if (line <= c->n0) { return CAM_LINE_BOUND; }
    *tick = on_grid(c, line);
    *run_lo = c->n0;
    *run_hi = c->n1;
    return CAM_LINE_EXACT;
}

uint64_t cam_track_floor(const cam_track_t *c, uint64_t line)
{
    if (!c->anchored || line < c->n0) { return c->start_tick; }
    return line <= c->n1 ? on_grid(c, line) : c->t1;
}

int cam_track_range(const cam_track_t *c, uint64_t first, uint64_t last, uint64_t *after, uint64_t *upto)
{
    if (last > cam_track_known(c)) { return 0; }
    *upto = last >= c->n0 ? on_grid(c, last) : c->t0;
    *after = cam_track_floor(c, first - 1u);
    return 1;
}
