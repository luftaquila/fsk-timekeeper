#include "protocol.h"

#include <string.h>

/* The existing packed integer fields use the nRF's little-endian layout.
 * Explicit helpers handle the fields whose wire width differs from the model. */
static void put48(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 6; i++) { p[i] = (uint8_t)(v >> (8u * i)); }
}

static uint64_t get48(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 6; i++) { v |= (uint64_t)p[i] << (8u * i); }
    return v;
}

/* Six slots, each one of 17 values: IDLE, REQUEST, DATA[0..4], OFFER[0..4],
 * CONFIRM[0..4]. Dense indices take 30 bits; the top two bits stay zero. */
static int schedule_code(uint8_t role)
{
    if (role == SLOT_ROLE_IDLE) { return 0; }
    if (role == SLOT_ROLE_REQUEST) { return 1; }
    unsigned i = SLOT_ENTRY(role);
    if (i >= MAX_NODES) { return -1; }
    switch (SLOT_ROLE(role)) {
    case SLOT_ROLE_DATA: return 2 + (int)i;
    case SLOT_ROLE_OFFER: return 7 + (int)i;
    case SLOT_ROLE_CONFIRM: return 12 + (int)i;
    default: return -1;
    }
}

static uint8_t schedule_role(unsigned code)
{
    if (code == 0) { return SLOT_ROLE_IDLE; }
    if (code == 1) { return SLOT_ROLE_REQUEST; }
    if (code < 7) { return (uint8_t)(SLOT_ROLE_DATA | (code - 2u)); }
    if (code < 12) { return (uint8_t)(SLOT_ROLE_OFFER | (code - 7u)); }
    return (uint8_t)(SLOT_ROLE_CONFIRM | (code - 12u));
}

int protocol_payload_size(uint8_t type, int model_len)
{
    switch (type) {
    case PKT_TYPE_BEACON:
        if (model_len == sizeof(beacon_pl_t)) { return PL_BEACON; }
        if (model_len == sizeof(admission_beacon_t)) { return PL_ADMISSION; }
        break;
    case PKT_TYPE_UPLINK: if (model_len == sizeof(uplink_pl_t)) { return PL_UPLINK; } break;
    case PKT_TYPE_JOIN: if (model_len == sizeof(join_pl_t)) { return PL_JOIN; } break;
    case PKT_TYPE_OFFER: if (model_len == sizeof(offer_pl_t)) { return PL_OFFER; } break;
    case PKT_TYPE_CONFIRM: if (model_len == sizeof(confirm_pl_t)) { return PL_CONFIRM; } break;
    }
    return -1;
}

#define UL_PACK_FLAGS (HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED | EVENT_TIME_UNKNOWN)

int protocol_encode(uint8_t *out, int cap, uint8_t type, const void *model, int model_len)
{
    int n = protocol_payload_size(type, model_len);
    if (n < 0 || cap < n) { return -1; }
    if (type == PKT_TYPE_BEACON) {
        const beacon_pl_t *b = model;
        out[0] = b->seq; out[1] = b->flags;
        put48(out + 2, b->m_tx_prev); out[8] = b->cp_req;
        memcpy(out + 9, b->slot, sizeof(b->slot));
        put48(out + 59, b->m_rx_prev); out[65] = b->rx_entry;
        if (n == PL_ADMISSION) {
            const admission_beacon_t *a = model;
            uint32_t packed = 0;
            for (unsigned i = 0; i < 6; i++) {
                int code = schedule_code(a->schedule[i]);
                if (code < 0) { return -1; }
                packed |= (uint32_t)code << (5u * i);
            }
            for (unsigned i = 0; i < 4; i++) { out[PL_BEACON + i] = (uint8_t)(packed >> (8u * i)); }
            out[PL_BEACON + 4] = a->pending;
        }
    } else if (type == PKT_TYPE_UPLINK) {
        const uplink_pl_t *u = model;
        if (u->kind > UL_KIND_CHECKPOINT || (u->flags & ~UL_PACK_FLAGS) || u->beacon_ctr > 0xffffffu) { return -1; }
        /* Flag bits 4/5 are unused; store the two-bit kind there. */
        out[0] = (uint8_t)(u->flags | (u->kind << 4));
        memcpy(out + 1, &u->ev_seq, 2);
        memcpy(out + 3, &u->master_boot_id, 8);
        memcpy(out + 11, &u->capture_seq, 4);
        put48(out + 15, u->tick);
        memcpy(out + 21, &u->sync_age_ms, 2);
        memcpy(out + 23, &u->u, 17);
        if (u->kind == UL_KIND_LOSS) {
            put48(out + 27, u->u.loss.end_tick);
            memset(out + 33, 0, 7); /* unused tail of the fixed-size union */
        }
        memcpy(out + 40, &u->diag, sizeof(u->diag));
        for (unsigned i = 0; i < 3; i++) { out[59 + i] = (uint8_t)(u->beacon_ctr >> (8u * i)); }
    } else {
        memcpy(out, model, (size_t)n);
    }
    return n;
}

int protocol_decode(void *model, int model_len, uint8_t type, const uint8_t *in, int len)
{
    int n = protocol_payload_size(type, model_len);
    if (n < 0 || len != n) { return -1; }
    if (type == PKT_TYPE_BEACON) {
        admission_beacon_t a = {0};
        a.base.seq = in[0]; a.base.flags = in[1];
        a.base.m_tx_prev = get48(in + 2); a.base.cp_req = in[8];
        memcpy(a.base.slot, in + 9, sizeof(a.base.slot));
        a.base.m_rx_prev = get48(in + 59); a.base.rx_entry = in[65];
        if (n == PL_ADMISSION) {
            uint32_t packed = 0;
            for (unsigned i = 0; i < 4; i++) { packed |= (uint32_t)in[PL_BEACON + i] << (8u * i); }
            if (packed >> 30) { return -1; }
            for (unsigned i = 0; i < 6; i++) {
                unsigned code = (packed >> (5u * i)) & 31u;
                if (code > 16u) { return -1; }
                a.schedule[i] = schedule_role(code);
            }
            a.pending = in[PL_BEACON + 4];
        }
        memcpy(model, &a, (size_t)model_len);
    } else if (type == PKT_TYPE_UPLINK) {
        if (in[0] & 0x80u) { return -1; }
        uplink_pl_t u = {0};
        u.kind = (in[0] >> 4) & 3u; u.flags = in[0] & UL_PACK_FLAGS;
        memcpy(&u.ev_seq, in + 1, 2);
        memcpy(&u.master_boot_id, in + 3, 8);
        memcpy(&u.capture_seq, in + 11, 4);
        u.tick = get48(in + 15);
        memcpy(&u.sync_age_ms, in + 21, 2);
        memcpy(&u.u, in + 23, 17);
        if (u.kind == UL_KIND_LOSS) {
            memset(&u.u, 0, sizeof(u.u));
            memcpy(&u.u.loss.end_seq, in + 23, 4);
            u.u.loss.end_tick = get48(in + 27);
        }
        memcpy(&u.diag, in + 40, sizeof(u.diag));
        for (unsigned i = 0; i < 3; i++) { u.beacon_ctr |= (uint32_t)in[59 + i] << (8u * i); }
        memcpy(model, &u, sizeof(u));
    } else {
        memcpy(model, in, (size_t)n);
    }
    return 0;
}
