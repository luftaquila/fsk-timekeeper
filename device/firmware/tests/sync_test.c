/* sync.c against the pre-split sensor timing code (characterization), plus the
 * held-capture interpolation added with it. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/protocol.h"
#include "../src/sync.h"

/* ---- reference: the formulas of main.c at ef62d5d, verbatim in behaviour ---- */
#define OFF_HIST 8u
typedef struct {
    uint64_t prev_l_rx, cur_off, sync_ref_tick;
    uint8_t prev_seq;
    int have_prev, have_off, skew_valid;
    int64_t off_hist[OFF_HIST];
    uint64_t lrx_hist[OFF_HIST];
    unsigned hist_n, hist_i;
    int32_t cur_skew;
    uint16_t rx_miss, beacon_gap;
} ref_t;

static void ref_beacon(ref_t *r, uint8_t seq, uint64_t m_tx_prev, uint64_t l_rx)
{
    if (r->have_prev) {
        if (seq == (uint8_t)(r->prev_seq + 1u)) {
            r->cur_off = m_tx_prev + T_AIR_REF_TICKS - r->prev_l_rx;
            r->sync_ref_tick = r->prev_l_rx;
            r->have_off = 1;
            r->beacon_gap = 0;
            r->off_hist[r->hist_i] = (int64_t)r->cur_off;
            r->lrx_hist[r->hist_i] = r->prev_l_rx;
            r->hist_i = (r->hist_i + 1u) % OFF_HIST;
            if (r->hist_n < OFF_HIST) { r->hist_n++; }
            if (r->hist_n >= 2u) {
                unsigned newest = (r->hist_i + OFF_HIST - 1u) % OFF_HIST;
                unsigned oldest = (r->hist_n < OFF_HIST) ? 0u : r->hist_i;
                int64_t doff = r->off_hist[newest] - r->off_hist[oldest];
                int64_t dl = (int64_t)(r->lrx_hist[newest] - r->lrx_hist[oldest]);
                if (dl >= (int64_t)SKEW_MIN_DL_TICKS) {
                    int64_t cand = (doff * 1000000) / dl;
                    r->cur_skew = (int32_t)(cand > 32767 ? 32767 : (cand < -32768 ? -32768 : cand));
                    r->skew_valid = (r->hist_n >= SKEW_MIN_SAMPLES) && (cand <= SKEW_CLAMP_PPM) && (cand >= -SKEW_CLAMP_PPM);
                }
            }
        } else {
            uint8_t miss = (uint8_t)(seq - r->prev_seq - 1u);
            if ((uint16_t)(r->rx_miss + miss) >= r->rx_miss) { r->rx_miss += miss; }
            r->beacon_gap = miss;
        }
    }
    r->prev_l_rx = l_rx;
    r->prev_seq = seq;
    r->have_prev = 1;
}

static uint64_t ref_to_master(const ref_t *r, uint64_t ev_tick)
{
    uint64_t mt = r->cur_off + ev_tick;
    if (r->skew_valid && ev_tick >= r->sync_ref_tick &&
        (ev_tick - r->sync_ref_tick) <= (uint64_t)SKEW_MAX_EXTRAP_MS * TICKS_PER_MS) {
        int64_t corr = (int64_t)(ev_tick - r->sync_ref_tick) * r->cur_skew / 1000000;
        if (corr >= 0) { mt += (uint64_t)corr; }
        else { mt -= (uint64_t)(-corr); }
    }
    return mt;
}

static uint16_t ref_age(const ref_t *r, uint64_t at)
{
    if (!r->have_off || at < r->sync_ref_tick) { return UINT16_MAX; }
    uint64_t age = (at - r->sync_ref_tick) / TICKS_PER_MS;
    return age > UINT16_MAX ? UINT16_MAX : (uint16_t)age;
}

/* ---- a clock model: master tick = master_base + true_us * 16; sensor tick
 * drifts by ppm relative to it. ---- */
static uint64_t sensor_tick(uint64_t true_us, int32_t ppm, uint64_t base)
{
    uint64_t t = true_us * 16u;
    int64_t drift = (int64_t)t * ppm / 1000000;
    return base + t + (uint64_t)drift;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void characterization(void)
{
    for (int trial = 0; trial < 200; trial++) {
        sync_t s;
        ref_t r;
        sync_init(&s);
        memset(&r, 0, sizeof(r));
        int32_t ppm = (int32_t)(rnd() % 161) - 80;
        uint64_t sbase = (uint64_t)rnd() * 97u, mbase = (uint64_t)rnd() * 131u;
        uint8_t seq = (uint8_t)rnd();
        uint64_t m_tx_prev = 0;
        for (int i = 0; i < 120; i++) {
            uint64_t true_us = (uint64_t)i * 1000000u + (rnd() % 3000u);
            uint64_t m_tx = mbase + true_us * 16u;
            uint64_t l_rx = sensor_tick(true_us, ppm, sbase);
            int lost = (rnd() % 10u) == 0u;
            if (!lost) {
                sync_beacon(&s, seq, l_rx, 1, 1, m_tx_prev);
                ref_beacon(&r, seq, m_tx_prev, l_rx);
            }
            m_tx_prev = m_tx;
            seq++;
            assert(s.cur.have == r.have_off);
            assert(s.skew_valid == r.skew_valid && s.skew_ppm == r.cur_skew);
            assert(s.rx_miss == r.rx_miss && s.beacon_gap == r.beacon_gap);
            if (r.have_off) {
                assert(s.cur.off == r.cur_off && s.cur.local == r.sync_ref_tick);
                for (int k = 0; k < 4; k++) {
                    uint64_t at = r.sync_ref_tick + (uint64_t)(rnd() % (9000u * TICKS_PER_MS));
                    assert(sync_to_master(&s, at) == ref_to_master(&r, at));
                    assert(sync_age_ms(&s, at) == ref_age(&r, at));
                }
            }
        }
    }
    printf("PASS characterization\n");
}

/* A clean session: 10 beacons at 1 s. */
static void warm(sync_t *s, int32_t ppm, uint64_t sbase, uint64_t mbase, unsigned beacons, uint64_t *m_tx_prev, uint8_t *seq)
{
    for (unsigned i = 0; i < beacons; i++) {
        uint64_t true_us = (uint64_t)i * 1000000u;
        sync_beacon(s, *seq, sensor_tick(true_us, ppm, sbase), 1, 1, *m_tx_prev);
        *m_tx_prev = mbase + true_us * 16u;
        (*seq)++;
    }
}

static void stamping_and_hold(void)
{
    const int32_t ppm = 37;
    const uint64_t sbase = 1000000007u, mbase = 555555555u;
    sync_t s;
    sync_init(&s);
    uint64_t m_tx_prev = 0;
    uint8_t seq = 250;
    warm(&s, ppm, sbase, mbase, 10, &m_tx_prev, &seq);
    assert(s.cur.have && s.skew_valid);
    assert(s.skew_ppm >= -ppm - 2 && s.skew_ppm <= -ppm + 2); /* a fast sensor clock: offset shrinks */

    /* Fresh anchor: stamped immediately, within the 1 ppm skew resolution
     * over the 1.5 s since the anchor (24 ticks) of the truth. */
    uint64_t t_us = 9500000u;
    uint64_t master;
    uint8_t flags;
    assert(sync_stamp(&s, sensor_tick(t_us, ppm, sbase), 1, &master, &flags) == SYNC_STAMPED);
    assert(flags == HEALTH_EVENT_REQUIRED);
    int64_t err = (int64_t)(master - (mbase + t_us * 16u));
    assert(err > -26 && err < 26);
    /* No HFXO: never stamped. */
    assert(sync_stamp(&s, sensor_tick(t_us, ppm, sbase), 0, &master, &flags) == SYNC_UNKNOWN);

    /* Beacons stop for 20 s: a capture 15 s after the last anchor is held ... */
    uint64_t gap_us = 8000000u + 15000000u;
    uint64_t held = sensor_tick(gap_us, ppm, sbase);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_HOLD);
    /* ... until two consecutive beacons make a new anchor; then it is interpolated. */
    sync_beacon(&s, seq + 19u, sensor_tick(29000000u, ppm, sbase), 1, 1, 0);
    sync_beacon(&s, (uint8_t)(seq + 20u), sensor_tick(30000000u, ppm, sbase), 1, 1, mbase + 29000000u * 16u);
    assert(s.cur.have && s.before.have);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_STAMPED);
    assert(flags == (HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED));
    err = (int64_t)(master - (mbase + gap_us * 16u));
    assert(err > -16 && err < 16);
    printf("PASS stamping_and_hold\n");
}

static void hold_limit(void)
{
    const int32_t ppm = -21;
    const uint64_t sbase = 77u, mbase = 99999u;
    sync_t s;
    sync_init(&s);
    uint64_t m_tx_prev = 0;
    uint8_t seq = 0;
    warm(&s, ppm, sbase, mbase, 10, &m_tx_prev, &seq);
    uint64_t held = sensor_tick(40000000u, ppm, sbase);
    uint64_t master;
    uint8_t flags;
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_HOLD);
    /* The next anchor comes 70 s after the last one: too far apart to trust. */
    sync_beacon(&s, 100, sensor_tick(79000000u, ppm, sbase), 1, 1, 0);
    sync_beacon(&s, 101, sensor_tick(80000000u, ppm, sbase), 1, 1, mbase + 79000000u * 16u);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_UNKNOWN);

    /* The clock was lost during a gap: no bracketing anchors survive. */
    sync_init(&s);
    m_tx_prev = 0;
    seq = 0;
    warm(&s, ppm, sbase, mbase, 10, &m_tx_prev, &seq);
    held = sensor_tick(20000000u, ppm, sbase);
    sync_clock_lost(&s);
    sync_beacon(&s, 30, sensor_tick(29000000u, ppm, sbase), 1, 1, 0);
    sync_beacon(&s, 31, sensor_tick(30000000u, ppm, sbase), 1, 1, mbase + 29000000u * 16u);
    assert(sync_stamp(&s, held, 1, &master, &flags) == SYNC_UNKNOWN);
    printf("PASS hold_limit\n");
}

static void invalid_inputs(void)
{
    sync_t s;
    sync_init(&s);
    /* A beacon whose RxDone was not captured cannot anchor the next one. */
    sync_beacon(&s, 1, 1000, 0, 1, 0);
    sync_beacon(&s, 2, 17000000, 1, 1, 16000000);
    assert(!s.cur.have);
    /* A beacon whose m_tx_prev is marked invalid does not anchor either. */
    sync_beacon(&s, 3, 33000000, 1, 0, 32000000);
    assert(!s.cur.have);
    sync_beacon(&s, 4, 49000000, 1, 1, 48000000);
    assert(s.cur.have);
    /* A new session keeps the miss counter only. */
    sync_beacon(&s, 9, 129000000, 1, 1, 0);
    assert(s.rx_miss == 4 && s.beacon_gap == 4);
    sync_session(&s);
    assert(!s.cur.have && !s.have_prev && s.rx_miss == 4 && s.beacon_gap == 0);
    printf("PASS invalid_inputs\n");
}

int main(void)
{
    characterization();
    stamping_and_hold();
    hold_limit();
    invalid_inputs();
    return 0;
}
