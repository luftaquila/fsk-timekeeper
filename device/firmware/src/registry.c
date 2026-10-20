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

void reg_slot_table(const registry_t *r, uint32_t now_ms, beacon_slot_t out[MAX_NODES])
{
    for (unsigned i = 0; i < MAX_NODES; i++) {
        const reg_entry_t *e = &r->e[i];
        if (e->used && reg_state(e, now_ms) != PU_STATE_LOST) {
            out[i].node_id = e->id;
            out[i].ack_seq = e->ack_seq;
            out[i].link_id = e->link_id;
        } else {
            out[i].node_id = 0;
            out[i].ack_seq = 0;
            out[i].link_id = 0;
        }
    }
}

void reg_session(registry_t *r)
{
    memset(r, 0, sizeof(*r));
}

unsigned reg_count(const registry_t *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) { if (r->e[i].used) { n++; } }
    return n;
}
