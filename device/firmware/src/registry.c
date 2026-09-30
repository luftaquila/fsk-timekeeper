#include "registry.h"

#include <string.h>

#include "config.h"
#include "proto_usb.h"

void reg_init(registry_t *r)
{
    memset(r, 0, sizeof(*r));
    for (unsigned i = 0; i < MAX_NODES; i++) { r->e[i].last_state = -1; }
}

int reg_state(const reg_entry_t *e, uint32_t now_ms)
{
    uint32_t age = now_ms - e->heard_ms;
    if (age <= LINK_OK_MS) { return PU_STATE_OK; }
    if (age <= LINK_STALE_MS) { return PU_STATE_STALE; }
    return PU_STATE_LOST;
}

reg_entry_t *reg_lookup(registry_t *r, uint32_t id)
{
    for (unsigned i = 0; i < MAX_NODES; i++) {
        if (r->e[i].used && r->e[i].id == id) { return &r->e[i]; }
    }
    return NULL;
}

reg_entry_t *reg_claim(registry_t *r, uint32_t id, uint32_t now_ms, int *why)
{
    uint16_t sid = node_short_id(id);
    int free_slot = -1, lost_slot = -1;
    for (unsigned i = 0; i < MAX_NODES; i++) {
        const reg_entry_t *e = &r->e[i];
        if (!e->used) {
            if (free_slot < 0) { free_slot = (int)i; }
        } else if (reg_state(e, now_ms) == PU_STATE_LOST) {
            if (lost_slot < 0) { lost_slot = (int)i; }
        } else if (e->short_id == sid) {
            *why = REG_COLLISION;
            return NULL;
        }
    }
    int slot = free_slot >= 0 ? free_slot : lost_slot;
    if (slot < 0) {
        *why = REG_FULL;
        return NULL;
    }
    reg_entry_t *e = &r->e[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->id = id;
    e->short_id = sid;
    e->heard_ms = now_ms;
    e->last_state = -1;
    return e;
}

void reg_slot_table(const registry_t *r, uint32_t now_ms, beacon_slot_t out[MAX_NODES])
{
    for (unsigned i = 0; i < MAX_NODES; i++) {
        const reg_entry_t *e = &r->e[i];
        if (e->used && reg_state(e, now_ms) != PU_STATE_LOST) {
            out[i].short_id = e->short_id;
            out[i].ack_seq = e->ack_seq;
            out[i].boot_tag = (uint8_t)e->sensor_boot;
        } else {
            out[i].short_id = 0;
            out[i].ack_seq = 0;
            out[i].boot_tag = 0;
        }
    }
}

void reg_session(registry_t *r)
{
    for (unsigned i = 0; i < MAX_NODES; i++) { r->e[i].have_boot = 0; }
}

unsigned reg_count(const registry_t *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) { if (r->e[i].used) { n++; } }
    return n;
}
