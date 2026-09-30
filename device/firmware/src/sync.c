#include "sync.h"

#include <string.h>

#include "config.h"
#include "protocol.h"

void sync_init(sync_t *s)
{
    memset(s, 0, sizeof(*s));
}

void sync_session(sync_t *s)
{
    uint16_t rx_miss = s->rx_miss;
    sync_init(s);
    s->rx_miss = rx_miss;
}

void sync_clock_lost(sync_t *s)
{
    s->have_prev = 0;
    s->cur.have = 0;
    s->before.have = 0;
    s->hist_n = 0;
    s->hist_i = 0;
    s->skew_valid = 0;
}

static void skew_update(sync_t *s)
{
    if (s->hist_n < 2u) { return; }
    unsigned newest = (s->hist_i + SYNC_HIST - 1u) % SYNC_HIST;
    unsigned oldest = (s->hist_n < SYNC_HIST) ? 0u : s->hist_i;
    int64_t doff = s->off_hist[newest] - s->off_hist[oldest];
    int64_t dl = (int64_t)(s->lrx_hist[newest] - s->lrx_hist[oldest]);
    if (dl < (int64_t)SKEW_MIN_DL_TICKS) { return; }
    int64_t cand = (doff * 1000000) / dl;
    /* Report the real value (an RC fallback reads ~10000 ppm); trust it for
     * stamping only when it is a plausible crystal drift. */
    s->skew_ppm = (int32_t)(cand > 32767 ? 32767 : (cand < -32768 ? -32768 : cand));
    s->skew_valid = (s->hist_n >= SKEW_MIN_SAMPLES) && (cand <= SKEW_CLAMP_PPM) && (cand >= -SKEW_CLAMP_PPM);
}

int sync_beacon(sync_t *s, uint8_t seq, uint64_t l_rx, int l_rx_ok, int m_tx_ok, uint64_t m_tx_prev)
{
    int anchored = 0;
    if (s->have_prev) {
        if (seq == (uint8_t)(s->prev_seq + 1u)) {
            if (s->prev_lrx_ok && m_tx_ok) {
                s->before = s->cur;
                s->cur.have = 1;
                s->cur.local = s->prev_lrx;
                s->cur.off = m_tx_prev + T_AIR_REF_TICKS - s->prev_lrx;
                s->off_hist[s->hist_i] = (int64_t)s->cur.off;
                s->lrx_hist[s->hist_i] = s->prev_lrx;
                s->hist_i = (s->hist_i + 1u) % SYNC_HIST;
                if (s->hist_n < SYNC_HIST) { s->hist_n++; }
                skew_update(s);
                anchored = 1;
            }
            s->beacon_gap = 0;
        } else {
            uint8_t miss = (uint8_t)(seq - s->prev_seq - 1u);
            if ((uint16_t)(s->rx_miss + miss) >= s->rx_miss) { s->rx_miss += miss; }
            s->beacon_gap = miss;
        }
    }
    s->have_prev = 1;
    s->prev_seq = seq;
    s->prev_lrx = l_rx;
    s->prev_lrx_ok = l_rx_ok;
    return anchored;
}

uint16_t sync_age_ms(const sync_t *s, uint64_t at)
{
    if (!s->cur.have || at < s->cur.local) { return UINT16_MAX; }
    uint64_t age = (at - s->cur.local) / TICKS_PER_MS;
    return age > UINT16_MAX ? UINT16_MAX : (uint16_t)age;
}

uint8_t sync_health(const sync_t *s, uint64_t at, int xtal)
{
    uint8_t flags = 0;
    if (sync_age_ms(s, at) <= SYNC_TTL_MS) { flags |= HEALTH_SYNC_VALID; }
    if (s->skew_valid) { flags |= HEALTH_SKEW_VALID; }
    if (xtal) { flags |= HEALTH_CLOCK_XTAL; }
    return flags;
}

uint64_t sync_to_master(const sync_t *s, uint64_t local)
{
    if (!s->cur.have) { return 0; }
    uint64_t mt = s->cur.off + local;
    /* Correct the drift since the anchor only forward of it, within the valid
     * sync window, and only with a validated skew (|skew| <= SKEW_CLAMP_PPM). */
    if (s->skew_valid && local >= s->cur.local &&
        (local - s->cur.local) <= (uint64_t)SKEW_MAX_EXTRAP_MS * TICKS_PER_MS) {
        int64_t corr = (int64_t)(local - s->cur.local) * s->skew_ppm / 1000000;
        if (corr >= 0) { mt += (uint64_t)corr; }
        else           { mt -= (uint64_t)(-corr); }
    }
    return mt;
}

int sync_stamp(const sync_t *s, uint64_t local, int xtal, uint64_t *master, uint8_t *flags)
{
    if (!s->cur.have || !xtal) { return SYNC_UNKNOWN; }
    if (local >= s->cur.local) {
        if (local - s->cur.local > (uint64_t)SYNC_TTL_MS * TICKS_PER_MS) { return SYNC_HOLD; }
        *master = sync_to_master(s, local);
        *flags = sync_health(s, local, xtal);
        return SYNC_STAMPED;
    }
    /* Older than the newest anchor: interpolate between the two anchors around it. */
    if (!s->before.have || local < s->before.local) { return SYNC_UNKNOWN; }
    uint64_t dl = s->cur.local - s->before.local;
    if (dl == 0 || dl > (uint64_t)SYNC_HOLD_MAX_MS * TICKS_PER_MS) { return SYNC_UNKNOWN; }
    uint64_t m1 = s->before.off + s->before.local;
    uint64_t m2 = s->cur.off + s->cur.local;
    uint64_t dm = m2 - m1;
    if (dm > 2u * dl) { return SYNC_UNKNOWN; } /* not a plausible rate: anchors disagree */
    uint64_t de = local - s->before.local;
    *master = m1 + (de * dm + dl / 2u) / dl;
    *flags = HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED;
    return SYNC_STAMPED;
}
