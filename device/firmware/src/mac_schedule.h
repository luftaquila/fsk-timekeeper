#ifndef MAC_SCHEDULE_H
#define MAC_SCHEDULE_H

#include "config.h"
#include "protocol.h"

/* A normal beacon's implicit schedule. Admission beacons carry this explicitly. */
static inline void mac_normal_schedule(const beacon_pl_t *b, uint8_t slots[SLOT_COUNT])
{
    unsigned live[MAX_NODES], n = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) { if (b->slot[i].node_id) { live[n++] = i; } }
    for (unsigned j = 0; j < SLOT_COUNT - 1u; j++) {
        slots[j] = n ? (uint8_t)(SLOT_ROLE_DATA | live[j % n]) : SLOT_ROLE_IDLE;
    }
    slots[SLOT_COUNT - 1u] = SLOT_ROLE_REQUEST;
}

static inline int mac_schedule_valid(const uint8_t slots[SLOT_COUNT])
{
    for (unsigned j = 0; j < SLOT_COUNT; j++) {
        unsigned role = SLOT_ROLE(slots[j]), entry = SLOT_ENTRY(slots[j]);
        if (role == SLOT_ROLE_IDLE || role == SLOT_ROLE_REQUEST) {
            if (entry) { return 0; }
        } else if (role == SLOT_ROLE_DATA || role == SLOT_ROLE_OFFER || role == SLOT_ROLE_CONFIRM) {
            if (entry >= MAX_NODES) { return 0; }
        } else { return 0; }
    }
    return 1;
}
#endif
