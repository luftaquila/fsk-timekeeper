/* Master registration policy. Owns candidates, replay-request history and the
 * admission schedule; the MAC owns radio I/O, deadlines and USB reporting.
 * Time is supplied by the caller. A verified CONFIRM installs a connection;
 * explicit forget removes it. Requests and scheduling leave active entries intact. */
#ifndef MASTER_ADMISSION_H
#define MASTER_ADMISSION_H

#include "config.h"
#include "registry.h"

typedef enum {
    ADMISSION_EMPTY = 0,
    ADMISSION_OFFER_READY,       /* no offer has started yet */
    ADMISSION_WAIT_CONFIRM,      /* confirmation scheduled; offer can be retried */
    ADMISSION_CONFIRM_NEXT_FRAME /* full fleet: reserve the next control slot */
} admission_phase_t;

typedef struct {
    uint32_t id, born_ms;
    uint64_t sensor_boot, link_id;
    uint8_t ns[16], nm[16];
    sec_link_keys_t keys;
    admission_phase_t phase;
    uint8_t offers;
} join_pending_t;

typedef struct { uint32_t id; uint64_t boot; uint8_t ns[16]; } join_seen_t;

typedef struct {
    join_pending_t pending[MAX_NODES];
    join_seen_t seen[8];
    uint8_t seen_count, seen_next, round_robin, quiet_frames, request_seen;
    uint64_t next_link;
    uint32_t join_until_ms;
} master_admission_t;

typedef enum {
    ADMISSION_IGNORED, ADMISSION_WAIT, ADMISSION_ACCEPTED,
    ADMISSION_FULL, ADMISSION_AUTH_FAILED
} admission_result_t;

void master_admission_open(master_admission_t *a, uint32_t now);
void master_admission_session(master_admission_t *a, uint32_t now);
int master_admission_forget(master_admission_t *a, registry_t *r, uint32_t id, uint32_t now);
/* MAC checks the protocol version first. id is set for WAIT/ACCEPTED notifications. */
admission_result_t master_admission_receive(master_admission_t *a, registry_t *r,
                                           const uint8_t *buf, int n, uint32_t now, uint32_t *id);
void master_admission_schedule(master_admission_t *a, const registry_t *r, uint32_t now,
                               admission_beacon_t *b, uint8_t slots[SLOT_COUNT]);
/* entry comes from a locally generated OFFER slot (always below MAX_NODES). */
int master_admission_offer(const master_admission_t *a, unsigned entry, uint32_t beacon_ctr,
                           offer_pl_t *offer);
/* Called only after TX starts. A paired confirmation uses the same frame. */
void master_admission_offer_started(master_admission_t *a, unsigned entry, int paired);

#endif
