/* Master's sensor registry (DESIGN §2.3), pure logic: entry k = slot-table entry k
 * (uplink slots are dealt among the live entries by mac_normal_schedule, mac_schedule.h). A sensor
 * is registered after mutual challenge confirmation; a LOST entry may be reclaimed. */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>

#include "protocol.h"
#include "secure.h"

typedef struct {
    int      used;
    uint32_t id;           /* 32-bit sender id */
    int      have_boot;    /* sensor_boot / ack_seq describe the current sensor boot */
    uint64_t sensor_boot;  /* boot_id from the uplink header */
    uint32_t link_id;
    uint8_t data_key[32];
    int have_ack;
    uint16_t ack_seq;      /* last ev_seq taken in order */
    uint32_t heard_ms;     /* last authenticated uplink */
    int      last_state;   /* PU_STATE_* last reported, -1 = none */
    sec_rx_counter_t rx;       /* replay window for this sensor */
    uint32_t sec_drop;     /* authenticated but refused by the replay window */
    /* diagnostics of the last uplink */
    float    rssi, snr;
    uint32_t lat_ms;
    ul_diag_t diag;
    uint8_t diag_valid;        /* a diagnostic uplink arrived on the current connection */
} reg_entry_t;

typedef struct {
    reg_entry_t e[MAX_NODES];
} registry_t;

void reg_init(registry_t *r);

/* PU_STATE_* from the time since the last uplink. */
int reg_state(const reg_entry_t *e, uint32_t now_ms);

reg_entry_t *reg_lookup(registry_t *r, uint32_t id);

/* Slot table for the next beacon: live entries only (LOST entries are free). */
void reg_slot_table(const registry_t *r, uint32_t now_ms, beacon_slot_t out[MAX_NODES]);

/* New master epoch: erase connection keys, ACKs and receive counters. */
void reg_session(registry_t *r);

unsigned reg_count(const registry_t *r);

#endif /* REGISTRY_H */
