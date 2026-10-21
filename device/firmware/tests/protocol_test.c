#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "config.h"

#define TICK48_MAX UINT64_C(0xffffffffffff) /* largest tick on the wire */

static void uplink_packing(void)
{
    uplink_pl_t u = {
        .kind = UL_KIND_EDGES, .flags = 0x4f, .ev_seq = 0x1234,
        .master_boot_id = UINT64_C(0xfedcba9876543210), .capture_seq = 0x89abcdef,
        .tick = UINT64_C(0x010203040506), .sync_age_ms = 0x7788,
        .u.edges = {5, {0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00}},
        .diag = {7, 0x0102, -18, 0x0304, 5, 0x0607, -10, 0x0809, 0x0a0b, 0x0c0d, 14},
        .beacon_ctr = 0xa1b2c3
    }, out;
    /* Independent layout: kind/flags, full boot id, six tick bytes, edge deltas, signed diags. */
    static const uint8_t expected[PL_UPLINK] = {
        0x5f,0x34,0x12,0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0xef,0xcd,0xab,0x89,
        6,5,4,3,2,1,0x88,0x77,5,0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55,
        0xcc,0xbb,0xaa,0x99,0,0xff,0xee,0xdd,
        7,2,1,0xee,0xff,4,3,5,7,6,0xf6,0xff,9,8,0x0b,0x0a,0x0d,0x0c,14,0xc3,0xb2,0xa1
    };
    uint8_t wire[PL_UPLINK];
    assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_UPLINK, &u, sizeof(u)) == sizeof(wire));
    assert(!memcmp(wire, expected, sizeof(wire)));
    assert(protocol_decode(&out, sizeof(out), PKT_TYPE_UPLINK, expected, sizeof(expected)) == 0);
    assert(!memcmp(&out, &u, sizeof(u)));

    unsigned seen[256] = {0};
    for (unsigned kind = 0; kind <= 3; kind++) {
        u.kind = (uint8_t)kind;
        memset(&u.u, 0, sizeof(u.u));
        for (unsigned flags = 0; flags < 256; flags++) {
            u.flags = (uint8_t)flags;
            int n = protocol_encode(wire, sizeof(wire), PKT_TYPE_UPLINK, &u, sizeof(u));
            if (flags & ~0x4fu) { assert(n < 0); continue; }
            assert(n == PL_UPLINK && !seen[wire[0]]++);
            assert(protocol_decode(&out, sizeof(out), PKT_TYPE_UPLINK, wire, n) == 0);
            assert(!memcmp(&u, &out, sizeof(u)));
        }
    }
    const uint64_t ticks[] = {0, 1, UINT32_MAX, UINT64_C(0x100000000),
                             UINT64_C(86400000) * TICKS_PER_MS, TICK48_MAX};
    u.kind = UL_KIND_LOSS; u.flags = 7; u.u.loss.end_seq = UINT32_MAX;
    for (unsigned i = 0; i < sizeof(ticks)/sizeof(*ticks); i++) {
        u.tick = ticks[i]; u.u.loss.end_tick = ticks[i];
        assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_UPLINK, &u, sizeof(u)) == PL_UPLINK);
        assert(protocol_decode(&out, sizeof(out), PKT_TYPE_UPLINK, wire, sizeof(wire)) == 0);
        assert(!memcmp(&u, &out, sizeof(u)));
    }
    uplink_pl_t saved = out;
    wire[0] |= 0x80;
    assert(protocol_decode(&out, sizeof(out), PKT_TYPE_UPLINK, wire, sizeof(wire)) < 0);
    assert(!memcmp(&out, &saved, sizeof(out)));
    assert(protocol_decode(&out, sizeof(out), PKT_TYPE_UPLINK, wire, sizeof(wire) - 1) < 0);
    u.kind = 4;
    assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_UPLINK, &u, sizeof(u)) < 0);
    u.kind = UL_KIND_SYNC; u.beacon_ctr = 0x1000000;
    assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_UPLINK, &u, sizeof(u)) < 0);
    puts("PASS compact_uplink_values_and_48bit_ticks");
}

static void beacon_packing(void)
{
    admission_beacon_t a = {
        .base = {.seq = 0x12, .flags = 3, .m_tx_prev = TICK48_MAX, .cp_req = 0x34,
                 .slot = {{0xfedc0001, 0x5678, UINT32_MAX}},
                 .m_rx_prev = UINT64_C(0x010203040506), .rx_entry = 4},
        .schedule = {SLOT_ROLE_IDLE, SLOT_ROLE_REQUEST, SLOT_ROLE_DATA,
                     SLOT_ROLE_DATA | 4, SLOT_ROLE_OFFER, SLOT_ROLE_CONFIRM | 4}, .pending = 0x1f
    }, out;
    uint8_t wire[PL_ADMISSION];
    const uint8_t prefix[] = {0x12,3,0xff,0xff,0xff,0xff,0xff,0xff,0x34,
                              1,0,0xdc,0xfe,0x78,0x56,0xff,0xff,0xff,0xff};
    const uint8_t schedule[] = {0x20,0x08,0x73,0x20,0x1f}; /* [0,1,2,6,7,16] in 5-bit fields */
    assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_BEACON, &a, sizeof(a)) == PL_ADMISSION);
    assert(!memcmp(wire, prefix, sizeof(prefix)) && !memcmp(wire + PL_BEACON, schedule, sizeof(schedule)));
    const uint8_t reply[] = {6,5,4,3,2,1,4};
    assert(!memcmp(wire + 59, reply, sizeof(reply)));
    assert(protocol_decode(&out, sizeof(out), PKT_TYPE_BEACON, wire, sizeof(wire)) == 0);
    assert(!memcmp(&a, &out, sizeof(a)));
    for (unsigned slot = 0; slot < 6; slot++) {
        for (unsigned value = 0; value < 256; value++) {
            a.schedule[slot] = (uint8_t)value;
            int valid = value == 0 || value == 16 || (value >= 8 && value <= 12) ||
                        (value >= 24 && value <= 28) || (value >= 32 && value <= 36);
            int n = protocol_encode(wire, sizeof(wire), PKT_TYPE_BEACON, &a, sizeof(a));
            if (!valid) { assert(n < 0); continue; }
            assert(n == PL_ADMISSION);
            assert(protocol_decode(&out, sizeof(out), PKT_TYPE_BEACON, wire, n) == 0);
            assert(!memcmp(&a, &out, sizeof(a)));
        }
        a.schedule[slot] = 0;
    }
    assert(protocol_encode(wire, sizeof(wire), PKT_TYPE_BEACON, &a, sizeof(a)) == PL_ADMISSION);
    admission_beacon_t saved = out;
    for (unsigned slot = 0; slot < 6; slot++) {
        for (unsigned code = 17; code < 32; code++) {
            uint32_t packed = code << (5u * slot);
            memcpy(wire + PL_BEACON, &packed, 4);
            assert(protocol_decode(&out, sizeof(out), PKT_TYPE_BEACON, wire, sizeof(wire)) < 0);
            assert(!memcmp(&out, &saved, sizeof(out)));
        }
    }
    for (unsigned bit = 30; bit < 32; bit++) {
        uint32_t packed = UINT32_C(1) << bit;
        memcpy(wire + PL_BEACON, &packed, 4);
        assert(protocol_decode(&out, sizeof(out), PKT_TYPE_BEACON, wire, sizeof(wire)) < 0);
        assert(!memcmp(&out, &saved, sizeof(out)));
    }
    beacon_pl_t base;
    assert(protocol_encode(wire, PL_BEACON, PKT_TYPE_BEACON, &a.base, sizeof(a.base)) == PL_BEACON);
    assert(protocol_decode(&base, sizeof(base), PKT_TYPE_BEACON, wire, PL_BEACON) == 0);
    assert(!memcmp(&base, &a.base, sizeof(base)));
    assert(protocol_encode(wire, PL_BEACON - 1, PKT_TYPE_BEACON, &a.base, sizeof(a.base)) < 0);
    puts("PASS compact_beacon_schedule_and_full_node_ids");
}

int main(void)
{
    unsigned seen[256] = {0};
    for (unsigned ver = 1; ver <= 32; ver++) {
        for (unsigned type = 1; type <= 5; type++) {
            assert(!seen[SEC_VT(ver, type)]++);
            assert(SEC_VT_VER(SEC_VT(ver, type)) == ver && SEC_VT_TYPE(SEC_VT(ver, type)) == type);
            if (ver <= 16) { assert(SEC_VT(ver, type) == (uint8_t)((ver << 4) | type)); }
        }
    }
    assert(SEC_VT(17, PKT_TYPE_BEACON) == 0x19 && SEC_VT(17, PKT_TYPE_CONFIRM) == 0x1d);
    assert(SEC_VT(32, PKT_TYPE_BEACON) == 0x09);
    assert(protocol_payload_size(PKT_TYPE_BEACON, -1) < 0 && protocol_payload_size(0, sizeof(beacon_pl_t)) < 0);
    uplink_packing(); beacon_packing();
    return 0;
}
