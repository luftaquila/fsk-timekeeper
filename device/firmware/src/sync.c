#include "sync.h"

#include <string.h>

#include "config.h"
#include "protocol.h"

/* Capture quantization and radio IRQ jitter of two anchors, in ticks (1 us). */
#define ANCHOR_JITTER_TICKS 16u

void sync_init(sync_t *s)
{
    memset(s, 0, sizeof(*s));
}

void sync_clock_lost(sync_t *s)
{
    s->have_prev = 0;
    s->cur.have = 0;
    s->before.have = 0;
    s->hist_n = 0;
    s->hist_i = 0;
    s->skew_valid = 0;
    s->skew_num = 0;
    s->skew_den = 0;
}

static void skew_update(sync_t *s)
{
    if (s->hist_n < 2u) { return; }
    unsigned newest = (s->hist_i + SYNC_HIST - 1u) % SYNC_HIST;
    unsigned oldest = newest, matching = 0;
    for (unsigned n = 0; n < s->hist_n; n++) {
        unsigned i = (newest + SYNC_HIST - n) % SYNC_HIST;
        if (s->air_hist[i] == s->air_hist[newest]) { oldest = i; matching++; }
    }
    if (matching < 2u) { return; }
    int64_t doff = s->off_hist[newest] - s->off_hist[oldest];
    int64_t dl = (int64_t)(s->lrx_hist[newest] - s->lrx_hist[oldest]);
    if (dl < (int64_t)SKEW_MIN_DL_TICKS) { return; }
    int64_t cand = (doff * 1000000) / dl;
    /* Report the real value (an RC fallback reads ~10000 ppm); trust it for
     * stamping only when it is a plausible crystal drift. */
    s->skew_ppm = (int32_t)(cand > 32767 ? 32767 : (cand < -32768 ? -32768 : cand));
    if (cand > SKEW_CLAMP_PPM || cand < -SKEW_CLAMP_PPM) { s->skew_valid = 0; return; }
    /* A format transition does not change the crystal. Retain the last valid
     * rate until enough samples of the new length replace it. Clock loss clears it. */
    if (matching >= SKEW_MIN_SAMPLES) {
        s->skew_num = doff;
        s->skew_den = (uint64_t)dl;
        s->skew_valid = 1;
    }
}

int sync_beacon(sync_t *s, uint8_t seq, uint64_t l_rx, int l_rx_ok, int m_tx_ok, uint64_t m_tx_prev,
                uint8_t air_class)
{
    int anchored = 0;
    if (s->have_prev) {
        if (seq == (uint8_t)(s->prev_seq + 1u)) {
            if (s->prev_lrx_ok && m_tx_ok) {
                s->off_hist[s->hist_i] = (int64_t)(m_tx_prev - s->prev_lrx);
                s->lrx_hist[s->hist_i] = s->prev_lrx;
                s->air_hist[s->hist_i] = s->prev_air_class;
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
    s->prev_air_class = air_class;
    return anchored;
}

int sync_roundtrip(sync_t *s, uint64_t m_tx, uint64_t l_rx, uint64_t l_tx, uint64_t m_rx)
{
    if (l_tx <= l_rx || m_rx <= m_tx) { return 0; }
    uint64_t dl = l_tx - l_rx, dm = m_rx - m_tx;
    uint64_t max_span = (uint64_t)(SLOT_OFFSET_MS + SLOT_COUNT * SLOT_LEN_MS) * TICKS_PER_MS;
    if (dl > max_span || dm > max_span) { return 0; }
    /* Sanity bound from the allowed crystal rate and slot guard, not a delay
     * calibration. Both clocks must advance over the same sub-second exchange. */
    uint64_t rate_error = dl * SKEW_CLAMP_PPM / 1000000u + ANCHOR_JITTER_TICKS;
    if (dm + rate_error < dl || dm > dl + rate_error + 2u * SLOT_GUARD_MS * TICKS_PER_MS) { return 0; }
    uint64_t local = l_rx + dl / 2u;
    if (s->cur.have && local <= s->cur.local) { return 0; }
    s->before = s->cur;
    s->cur = (sync_anchor_t){.have = 1, .local = local, .off = m_tx + dm / 2u - local};
    return 1;
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
    if (s->skew_valid && s->skew_den && local >= s->cur.local &&
        (local - s->cur.local) <= (uint64_t)SKEW_MAX_EXTRAP_MS * TICKS_PER_MS) {
        int64_t corr = (int64_t)(local - s->cur.local) * s->skew_num / (int64_t)s->skew_den;
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
    /* The two crystals run within SKEW_CLAMP_PPM of each other, as the skew check
     * demands on the normal path; anchors implying another rate disagree. */
    uint64_t dev = dm > dl ? dm - dl : dl - dm;
    if (dev > dl * (uint64_t)SKEW_CLAMP_PPM / 1000000u + ANCHOR_JITTER_TICKS) { return SYNC_UNKNOWN; }
    uint64_t de = local - s->before.local;
    *master = m1 + (de * dm + dl / 2u) / dl;
    *flags = HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED;
    return SYNC_STAMPED;
}
