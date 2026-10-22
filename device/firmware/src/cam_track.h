/* Camera line timing (pure logic). An anchor pairs a strobe count n (= camera line number, 1..)
 * with the TIMER1 tick of that exposure start. While anchors stay on the period grid (within
 * CAM_JITTER_TICKS) every line from the first anchor on starts exposing at
 * t0 + (line - n0) * period. An anchor off the grid either way means the strobe count no longer
 * matches the camera's lines: late is a skipped trigger or a strobe the master missed, early an
 * extra exposure or a counted glitch, and the master cannot tell which. Broken. */
#ifndef CAM_TRACK_H
#define CAM_TRACK_H

#include <stdint.h>

#include "config.h"

typedef struct {
    uint32_t period;
    uint64_t start_tick;  /* session start: no exposure before it */
    int anchored;         /* first anchor taken */
    uint64_t n0, t0;      /* first anchor */
    uint64_t n1, t1;      /* last anchor */
    int broken;           /* an anchor off the grid */
} cam_track_t;

void cam_track_init(cam_track_t *c, uint32_t period, uint64_t start_tick);

/* Anchor (n, t), n above the last anchor's. */
void cam_track_anchor(cam_track_t *c, uint64_t n, uint64_t t);

/* Line of the last anchor; 0 before the first. */
uint64_t cam_track_known(const cam_track_t *c);

#define CAM_LINE_EXACT 0 /* line - 1 and line anchored: tick exact, run_lo..run_hi the anchored lines */
#define CAM_LINE_BOUND 1 /* at or before the first anchor: bound it with cam_track_range() */
#define CAM_LINE_WAIT  2 /* past the last anchor: ask again after the next one */
int cam_track_line(const cam_track_t *c, uint64_t line, uint64_t *tick, uint64_t *run_lo, uint64_t *run_hi);

/* Ticks (*after, *upto] holding the exposures of lines first..last and the time between line
 * first - 1 and them; 0 while no anchor bounds line last from above. */
int cam_track_range(const cam_track_t *c, uint64_t first, uint64_t last, uint64_t *after, uint64_t *upto);

/* A tick at or before the exposure of line (exact once anchored). */
uint64_t cam_track_floor(const cam_track_t *c, uint64_t line);

#endif /* CAM_TRACK_H */
