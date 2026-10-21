#include "master_admission.h"

#include <string.h>

#include "mac_schedule.h"
#include "proto_usb.h" /* PU_STATE_LOST */

static void open_window(master_admission_t *a, uint32_t now)
{
    a->join_until_ms = now + JOIN_WINDOW_MS;
    a->quiet_frames = 0;
}

void master_admission_session(master_admission_t *a, uint32_t now)
{
    memset(a->pending, 0, sizeof(a->pending));
    memset(a->seen, 0, sizeof(a->seen));
    a->seen_count = a->seen_next = 0;
    a->next_link = 0;
    open_window(a, now);
}

static int join_open(const master_admission_t *a, uint32_t now)
{
    return (int32_t)(a->join_until_ms - now) > 0;
}

static admission_result_t on_join(master_admission_t *a, registry_t *r, const uint8_t *buf, int n,
                                  uint32_t now, uint32_t *id)
{
    join_pl_t j; sec_meta_t meta;
    if (sec_unseal(buf, n, &meta, &j, sizeof(j)) || !meta.node_id ||
        j.master_boot != sec_boot_id()) { return ADMISSION_AUTH_FAILED; }
    for (unsigned i = 0; i < MAX_NODES; i++) {
        if (a->pending[i].phase != ADMISSION_EMPTY && a->pending[i].id == meta.node_id) { return ADMISSION_IGNORED; }
    }
    for (unsigned i = 0; i < a->seen_count; i++) {
        if (a->seen[i].id == meta.node_id && a->seen[i].boot == meta.boot_id &&
            !memcmp(a->seen[i].ns, j.ns, 16)) { return ADMISSION_IGNORED; }
    }
    int slot = -1;
    reg_entry_t *old = reg_lookup(r, meta.node_id);
    if (old) { slot = (int)(old - r->e); }
    else {
        for (unsigned i = 0; i < MAX_NODES; i++) {
            if (a->pending[i].phase == ADMISSION_EMPTY && (!r->e[i].used || reg_state(&r->e[i], now) == PU_STATE_LOST)) {
                slot = (int)i; break;
            }
        }
    }
    if (slot < 0) { return ADMISSION_FULL; }
    if (a->pending[slot].phase != ADMISSION_EMPTY) { return ADMISSION_IGNORED; }
    join_pending_t candidate = {0};
    if (!sec_challenge(candidate.nm)) { return ADMISSION_IGNORED; }
    candidate.id = meta.node_id; candidate.sensor_boot = meta.boot_id;
    candidate.link_id = ++a->next_link; candidate.born_ms = now;
    memcpy(candidate.ns, j.ns, 16);
    if (!sec_link_keys(&candidate.keys, sec_boot_id(), candidate.id, candidate.sensor_boot,
                      candidate.ns, candidate.nm, candidate.link_id)) { return ADMISSION_IGNORED; }
    candidate.phase = ADMISSION_OFFER_READY;
    a->pending[slot] = candidate;
    join_seen_t *seen = &a->seen[a->seen_next];
    seen->id = meta.node_id; seen->boot = meta.boot_id; memcpy(seen->ns, j.ns, 16);
    a->seen_next = (uint8_t)((a->seen_next + 1u) % 8u);
    if (a->seen_count < 8u) { a->seen_count++; }
    /* A new challenge gets its full lifetime even at the end of a window. */
    if (!join_open(a, now)) { open_window(a, now); }
    else if ((int32_t)(a->join_until_ms - now) < (int32_t)JOIN_LIFETIME_MS) {
        a->join_until_ms = now + JOIN_LIFETIME_MS;
    }
    a->request_seen = 1;
    *id = meta.node_id;
    return ADMISSION_WAIT;
}

static admission_result_t on_confirm(master_admission_t *a, registry_t *r, const uint8_t *buf, int n,
                                     uint32_t now, uint32_t *accepted_id)
{
    if (n != WIRE_CONFIRM) { return ADMISSION_IGNORED; }
    uint32_t id; memcpy(&id, buf + 12, 4);
    for (unsigned i = 0; i < MAX_NODES; i++) {
        join_pending_t *p = &a->pending[i];
        if (p->phase == ADMISSION_EMPTY || p->phase == ADMISSION_OFFER_READY || p->id != id) { continue; }
        confirm_pl_t c; sec_meta_t meta;
        if (sec_unseal_key(p->keys.confirm, buf, n, &meta, &c, sizeof(c)) ||
            meta.boot_id != p->sensor_boot || c.master_boot != sec_boot_id() ||
            memcmp(c.nm, p->nm, 16)) { return ADMISSION_AUTH_FAILED; }
        if ((uint32_t)(now - p->born_ms) >= JOIN_LIFETIME_MS || !join_open(a, now)) { return ADMISSION_IGNORED; }
        reg_entry_t *e = &r->e[i];
        /* A different incumbent may have returned since this slot was reserved. */
        if (e->used && e->id != id && reg_state(e, now) != PU_STATE_LOST) {
            memset(p, 0, sizeof(*p)); return ADMISSION_IGNORED;
        }
        if (!e->used || e->id != id || e->sensor_boot != meta.boot_id) {
            memset(e, 0, sizeof(*e)); e->last_state = -1;
        }
        e->used = e->have_boot = 1; e->id = id; e->sensor_boot = meta.boot_id;
        e->link_id = p->link_id; e->heard_ms = now;
        /* CONFIRM proves liveness, not clock health; keep this boot's ACK, drop the diagnostics. */
        e->diag.health = 0;
        e->diag.sync_age_ms = UINT16_MAX;
        e->diag_valid = 0;
        memcpy(e->data_key, p->keys.data, 32);
        memset(&e->rx, 0, sizeof(e->rx));
        memset(p, 0, sizeof(*p));
        *accepted_id = id;
        return ADMISSION_ACCEPTED;
    }
    return ADMISSION_IGNORED;
}

admission_result_t master_admission_receive(master_admission_t *a, registry_t *r,
                                           const uint8_t *buf, int n, uint32_t now, uint32_t *id)
{
    if (n < 1) { return ADMISSION_IGNORED; }
    switch (SEC_VT_TYPE(buf[0])) {
    case PKT_TYPE_JOIN: return on_join(a, r, buf, n, now, id);
    case PKT_TYPE_CONFIRM: return on_confirm(a, r, buf, n, now, id);
    default: return ADMISSION_IGNORED;
    }
}

static unsigned expire_candidates(master_admission_t *a, uint32_t now, int open)
{
    unsigned candidates = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) {
        join_pending_t *p = &a->pending[i];
        if (p->phase != ADMISSION_EMPTY && (!open || (uint32_t)(now - p->born_ms) >= JOIN_LIFETIME_MS ||
                        (p->offers >= JOIN_MAX_OFFERS && p->phase != ADMISSION_CONFIRM_NEXT_FRAME))) {
            memset(p, 0, sizeof(*p));
        }
        if (p->phase != ADMISSION_EMPTY) { candidates++; }
    }
    return candidates;
}

/* Uses only the slots left after DATA; a full fleet alternates OFFER and CONFIRM across frames. */
static void control_slots(master_admission_t *a, unsigned live, uint8_t slots[SLOT_COUNT], unsigned j)
{
    unsigned first = a->round_robin;
    if (live == MAX_NODES) {
        for (unsigned i = 0; i < MAX_NODES; i++) {
            if (a->pending[i].phase == ADMISSION_CONFIRM_NEXT_FRAME) { first = i; break; }
        }
    }
    for (unsigned k = 0; k < MAX_NODES && j < SLOT_COUNT; k++) {
        unsigned i = (first + k) % MAX_NODES;
        join_pending_t *p = &a->pending[i];
        if (p->phase == ADMISSION_EMPTY) { continue; }
        if (SLOT_COUNT - j == 1u && live < MAX_NODES) { break; }
        if (live == MAX_NODES && p->phase == ADMISSION_CONFIRM_NEXT_FRAME) {
            slots[j++] = (uint8_t)(SLOT_ROLE_CONFIRM | i);
            p->phase = ADMISSION_WAIT_CONFIRM;
        } else {
            slots[j++] = (uint8_t)(SLOT_ROLE_OFFER | i);
            if (j < SLOT_COUNT) { slots[j++] = (uint8_t)(SLOT_ROLE_CONFIRM | i); }
        }
        a->round_robin = (uint8_t)((i + 1u) % MAX_NODES);
    }
}

void master_admission_schedule(master_admission_t *a, const registry_t *r, uint32_t now,
                               admission_beacon_t *b, uint8_t slots[SLOT_COUNT])
{
    int open = join_open(a, now);
    unsigned candidates = expire_candidates(a, now, open), live = 0;
    reg_slot_table(r, now, b->base.slot);
    for (unsigned i = 0; i < MAX_NODES; i++) { if (b->base.slot[i].node_id) { live++; } }
    if (!candidates && !a->request_seen) { if (a->quiet_frames < 3u) { a->quiet_frames++; } }
    else { a->quiet_frames = 0; }
    a->request_seen = 0;
    if (!candidates && (live == MAX_NODES || a->quiet_frames >= 3u)) { open = 0; a->join_until_ms = now; }
    memset(slots, 0, SLOT_COUNT);
    if (!open) { mac_normal_schedule(&b->base, slots); return; }
    b->base.flags |= BEACON_ADMISSION;
    unsigned j = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) {
        if (b->base.slot[i].node_id) { slots[j++] = (uint8_t)(SLOT_ROLE_DATA | i); }
        if (a->pending[i].phase != ADMISSION_EMPTY) {
            b->pending |= (uint8_t)(1u << i);
            if (!b->base.slot[i].node_id) {
                b->base.slot[i].node_id = a->pending[i].id;
                b->base.slot[i].link_id = a->pending[i].link_id;
            }
        }
    }
    if (!candidates) {
        for (; j < SLOT_COUNT; j++) { slots[j] = SLOT_ROLE_REQUEST; }
    } else {
        control_slots(a, live, slots, j);
    }
    memcpy(b->schedule, slots, SLOT_COUNT);
}

int master_admission_offer(const master_admission_t *a, unsigned entry, uint32_t beacon_ctr,
                           offer_pl_t *offer)
{
    const join_pending_t *p = &a->pending[entry];
    if (p->phase == ADMISSION_EMPTY) { return 0; }
    *offer = (offer_pl_t){ .node_id = p->id, .sensor_boot = p->sensor_boot, .link_id = p->link_id };
    memcpy(offer->ns, p->ns, 16); memcpy(offer->nm, p->nm, 16);
    for (unsigned k = 0; k < 3; k++) { offer->beacon_ctr[k] = (uint8_t)(beacon_ctr >> (8*k)); }
    return 1;
}

void master_admission_offer_started(master_admission_t *a, unsigned entry, int paired)
{
    join_pending_t *p = &a->pending[entry];
    p->offers++;
    p->phase = paired ? ADMISSION_WAIT_CONFIRM : ADMISSION_CONFIRM_NEXT_FRAME;
}
