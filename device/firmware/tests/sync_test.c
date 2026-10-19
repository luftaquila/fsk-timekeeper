/* Independent clock models: symmetric delay cancels; asymmetry leaves half
 * the difference. Beacons estimate rate but cannot establish an offset alone. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "protocol.h"
#include "sync.h"

static uint64_t local(uint64_t us, int32_t ppb, uint64_t base)
{
    uint64_t t = us * 16u;
    return base + t + (uint64_t)((int64_t)t * ppb / 1000000000);
}
static uint32_t random_state = 12345;
static uint32_t random32(void)
{
    random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5;
    return random_state;
}
static void near(uint64_t got, uint64_t want, int64_t tolerance)
{
    int64_t error = (int64_t)(got - want);
    assert(error >= -tolerance && error <= tolerance);
}

static void clock_models(void)
{
    unsigned checked = 0;
    for (unsigned trial = 0; trial < 200; trial++) {
        sync_t s; sync_init(&s);
        int32_t ppb = (int32_t)(random32() % 160001) - 80000;
        uint64_t sb = (uint64_t)random32() * 97u, mb = (uint64_t)random32() * 131u;
        /* Also run near one-day uptime, across the 8-bit beacon wrap. */
        uint64_t start_us = trial & 1 ? UINT64_C(86000000000) : 0;
        uint8_t seq = 250;
        uint64_t m1 = 0, l2 = 0, l3 = 0, m4 = 0;
        int reply = 0;
        int asymmetry_us = (int)(trial % 21u) - 10;
        for (unsigned i = 0; i < 120; i++, seq++) {
            unsigned format = (i / 30u) % 2u;
            unsigned delay_us = format ? 178u : 144u;
            uint64_t us = start_us + (uint64_t)i * 1000000u + random32() % 3000u;
            int received = random32() % 10u != 0;
            if (received) {
                sync_beacon(&s, seq, local(us + delay_us, ppb, sb), 1, i != 0, m1, (uint8_t)format);
                if (reply) { assert(sync_roundtrip(&s, m1, l2, l3, m4)); }
            }
            reply = received && seq % 5u == 0 && random32() % 10u != 0;
            m1 = mb + us * 16u;
            l2 = local(us + delay_us, ppb, sb);
            uint64_t tx_us = us + delay_us + 150000u + random32() % 550000u;
            l3 = local(tx_us, ppb, sb);
            m4 = mb + (tx_us + delay_us + asymmetry_us) * 16u;
            uint64_t event_us = us + 800000u, master;
            uint8_t flags;
            if (s.skew_valid && sync_stamp(&s, local(event_us, ppb, sb), 1, &master, &flags) == SYNC_STAMPED &&
                flags == HEALTH_EVENT_REQUIRED) {
                near(master, mb + event_us * 16u + (uint64_t)(asymmetry_us * 8), 5);
                checked++;
            }
        }
    }
    assert(checked > 10000);
    puts("PASS two_way_clock_models_delay_asymmetry_fractional_rate_day_uptime");
}

static void warm(sync_t *s, int32_t ppb, uint64_t sb, uint64_t mb)
{
    for (unsigned i = 0; i < 10; i++) {
        uint64_t us = (uint64_t)i * 1000000u;
        sync_beacon(s, (uint8_t)i, local(us + 144, ppb, sb), 1, i != 0,
                     i ? mb + (us - 1000000u) * 16u : 0, 0);
        if (i) {
            uint64_t prev = us - 1000000u;
            assert(sync_roundtrip(s, mb + prev * 16u, local(prev + 144, ppb, sb),
                                  local(prev + 100144, ppb, sb), mb + (prev + 100288) * 16u));
        }
    }
}

static void stamping_and_hold(void)
{
    const int32_t ppb = 37500; /* display truncates to ppm; conversion must not */
    const uint64_t sb = 1000000007u, mb = 555555555u;
    sync_t s; sync_init(&s); warm(&s, ppb, sb, mb);
    assert(s.cur.have && s.skew_valid);
    uint64_t master; uint8_t flags;
    assert(sync_stamp(&s, local(14000000u, ppb, sb), 1, &master, &flags) == SYNC_STAMPED);
    assert(flags == HEALTH_EVENT_REQUIRED);
    near(master, mb + 14000000u * 16u, 3);
    assert(sync_stamp(&s, local(14000000u, ppb, sb), 0, &master, &flags) == SYNC_UNKNOWN);
    uint64_t held = local(23000000u, ppb, sb);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_HOLD);
    sync_beacon(&s, 29, local(29000144u, ppb, sb), 1, 1, 0, 0);
    sync_beacon(&s, 30, local(30000144u, ppb, sb), 1, 1, mb + 29000000u * 16u, 0);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_HOLD);
    assert(sync_roundtrip(&s, mb + 29000000u * 16u, local(29000144u, ppb, sb),
                          local(29100144u, ppb, sb), mb + 29100288u * 16u));
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_STAMPED);
    assert(flags == (HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED));
    near(master, mb + 23000000u * 16u, 3);
    puts("PASS two_way_stamping_and_hold");
}

static void hold_limit(void)
{
    const int32_t ppb = -21000;
    const uint64_t sb = 77u, mb = 99999u;
    sync_t s; sync_init(&s); warm(&s, ppb, sb, mb);
    uint64_t held = local(40000000u, ppb, sb), master; uint8_t flags;
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_HOLD);
    assert(sync_roundtrip(&s, mb + 79000000u * 16u, local(79000144u, ppb, sb),
                          local(79100144u, ppb, sb), mb + 79100288u * 16u));
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_UNKNOWN);
    sync_clock_lost(&s);
    assert(!s.cur.have && !s.before.have && !s.skew_valid && !s.skew_den);
    assert(sync_roundtrip(&s, mb + 80000000u * 16u, local(80000144u, ppb, sb),
                          local(80100144u, ppb, sb), mb + 80100288u * 16u));
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_UNKNOWN);
    puts("PASS two_way_hold_limit_clock_loss");
}

static void interpolation_rate(void)
{
    const uint64_t dl = 20u * 16000000u;
    const struct { int64_t dev; int ok; } cases[] = {
        {(int64_t)(dl * 90u / 1000000u),1}, {-(int64_t)(dl * 90u / 1000000u),1},
        {(int64_t)(dl * 110u / 1000000u),0}, {-(int64_t)(dl * 110u / 1000000u),0}, {-(int64_t)dl - 16000000,0}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++) {
        sync_t s; sync_init(&s);
        s.before = (sync_anchor_t){1, 1000000000u, 4242};
        s.cur = (sync_anchor_t){1, s.before.local + dl, s.before.off + (uint64_t)cases[i].dev};
        uint64_t master; uint8_t flags;
        assert(sync_stamp(&s, s.before.local + dl/4, 1, &master, &flags) == (cases[i].ok ? SYNC_STAMPED : SYNC_UNKNOWN));
        if (cases[i].ok) { near(master, s.before.off + s.before.local + dl/4 + (uint64_t)(cases[i].dev/4), 1); }
    }
    puts("PASS interpolation_rate");
}

static void invalid_inputs(void)
{
    sync_t s; sync_init(&s);
    sync_beacon(&s, 1, 1000, 0, 1, 0, 0);
    sync_beacon(&s, 2, 17000000, 1, 1, 16000000, 0);
    assert(!s.hist_n && !s.cur.have);
    sync_beacon(&s, 3, 33000000, 1, 0, 32000000, 0);
    assert(!s.hist_n);
    sync_beacon(&s, 4, 49000000, 1, 1, 48000000, 0);
    assert(s.hist_n == 1 && !s.cur.have);
    for (unsigned i = 5; i < 10; i++) { sync_beacon(&s, (uint8_t)i, (i-1)*16000000u + 1000000u, 1, 1, (i-1)*16000000u, 0); }
    assert(s.skew_valid && !(sync_health(&s, 145000000, 1) & HEALTH_SYNC_VALID));
    assert(!sync_roundtrip(&s, 100000, 200000, 200000, 1700000));
    assert(!sync_roundtrip(&s, 100000, 200000, 1800000, 100000));
    assert(!sync_roundtrip(&s, 100000, 200000, 1800000, 1600000));
    assert(!sync_roundtrip(&s, 100000, 200000, 1800000, 2000000));
    assert(!sync_roundtrip(&s, 100000, 200000, 20000000, 20000000));
    assert(sync_roundtrip(&s, 100000, 200000, 1800000, 1700000));
    assert(!sync_roundtrip(&s, 100000, 200000, 1800000, 1700000));
    sync_beacon(&s, 14, 209000000, 1, 1, 0, 0);
    assert(s.rx_miss == 4 && s.beacon_gap == 4);
    sync_session(&s);
    assert(!s.cur.have && !s.have_prev && s.rx_miss == 4 && s.beacon_gap == 0);
    puts("PASS two_way_invalid_inputs_and_downlink_only");
}

int main(void)
{
    clock_models(); stamping_and_hold(); hold_limit(); interpolation_rate(); invalid_inputs();
    return 0;
}
