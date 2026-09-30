/* Master's sensor registry (DESIGN §2.3): entry k = uplink slot k. A sensor is
 * registered on its first authenticated uplink; a LOST entry may be reclaimed.
 * Pure logic. */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>

#include "protocol.h"
#include "secure.h"

typedef struct {
    int      used;
    uint32_t id;           /* 32-bit sender id */
    uint16_t short_id;
    int      have_boot;    /* sensor_boot / ack_seq describe the current sensor boot */
    uint32_t sensor_boot;  /* boot_id from the uplink header */
    uint16_t ack_seq;      /* last ev_seq taken in order */
    uint32_t heard_ms;     /* last authenticated uplink */
    int      last_state;   /* PU_STATE_* last reported, -1 = none */
    sec_replay_t rx;       /* replay window for this sensor */
    uint32_t sec_drop;     /* authenticated but refused (replay, other session) */
    /* diagnostics of the last uplink */
    float    rssi, snr;
    uint32_t lat_ms;
    uint8_t  health;
    uint16_t sync_age_ms;
    ul_diag_t diag;
} reg_entry_t;

typedef struct {
    reg_entry_t e[MAX_NODES];
} registry_t;

#define REG_FULL      1
#define REG_COLLISION 2

void reg_init(registry_t *r);

/* PU_STATE_* from the time since the last uplink. */
int reg_state(const reg_entry_t *e, uint32_t now_ms);

reg_entry_t *reg_lookup(registry_t *r, uint32_t id);

/* Entry for a new id: a free entry, else a LOST one. NULL with *why = REG_FULL,
 * or REG_COLLISION when a live sensor already uses its short id. */
reg_entry_t *reg_claim(registry_t *r, uint32_t id, uint32_t now_ms, int *why);

/* Slot table for the next beacon: live entries only (LOST slots are free). */
void reg_slot_table(const registry_t *r, uint32_t now_ms, beacon_slot_t out[MAX_NODES]);

/* New master session: every sensor keeps its slot and re-syncs its ev_seq with
 * its next uplink. */
void reg_session(registry_t *r);

unsigned reg_count(const registry_t *r);

#endif /* REGISTRY_H */
