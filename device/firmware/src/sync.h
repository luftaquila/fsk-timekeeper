/* Sensor-side time sync to the master timebase. Pure logic.
 * Frequency: beacon N carries the master TxDone of beacon N-1, paired with our RxDone of N-1.
 * Offset: a selected uplink closes a two-way exchange; the local RxDone/TxDone midpoint pairs
 * with the master TxDone/RxDone midpoint. Symmetric delay cancels, half of any asymmetry
 * remains; no fixed delay is used. A capture is stamped at once from a fresh anchor; one taken
 * while it is stale is held and interpolated once the next anchor exists.
 */
#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

#define SYNC_HIST 8u /* one-way beacon samples kept for the skew estimate */

typedef struct {
    int      have;   /* anchor valid */
    uint64_t local;  /* sensor tick at the midpoint of a two-way exchange */
    uint64_t off;    /* master tick - local tick at the anchor, mod 2^64 */
} sync_anchor_t;

typedef struct {
    int      have_prev;     /* prev_seq is known */
    uint8_t  prev_seq;
    int      prev_lrx_ok;   /* prev_lrx holds the RxDone of beacon prev_seq */
    uint64_t prev_lrx;
    uint8_t  prev_air_class; /* distinguish normal/admission packet lengths */
    sync_anchor_t cur;      /* newest anchor */
    sync_anchor_t before;   /* the anchor cur replaced (interpolation of older captures) */
    int64_t  off_hist[SYNC_HIST];
    uint64_t lrx_hist[SYNC_HIST];
    uint8_t air_hist[SYNC_HIST];
    unsigned hist_n, hist_i;
    int32_t  skew_ppm;      /* measured, clamped only to the i16 wire range */
    int      skew_valid;    /* plausible and from enough samples */
    int64_t  skew_num;      /* fractional offset slope; ppm is display only */
    uint64_t skew_den;
    uint16_t rx_miss;       /* misses since boot, saturating; long/ambiguous gaps excluded */
    uint8_t  beacon_gap;    /* last measured gap between received beacons, not a live outage */
} sync_t;

/* Boot: everything zero. */
void sync_init(sync_t *s);

/* HFXO not running: anchors and skew no longer describe the local clock. */
void sync_clock_lost(sync_t *s);

/* One authenticated session beacon; l_rx_ok: RxDone captured, m_tx_ok: m_tx_prev is seq-1's TxDone,
 * air_class: packet length class (only equal ones pair). Returns 1 for a new frequency sample. */
int sync_beacon(sync_t *s, uint8_t seq, uint64_t l_rx, int l_rx_ok, int m_tx_ok, uint64_t m_tx_prev,
                uint8_t air_class);

/* MAC matches the 4 authenticated captures to one frame/link, equal lengths; 1 = new anchor. */
int sync_roundtrip(sync_t *s, uint64_t m_tx, uint64_t l_rx, uint64_t l_tx, uint64_t m_rx);

/* Health bits (HEALTH_*) of a capture at local tick `at` with HFXO state xtal. */
uint8_t sync_health(const sync_t *s, uint64_t at, int xtal);

/* Age of the newest anchor at local tick `at`, ms, saturated (UINT16_MAX = none). */
uint16_t sync_age_ms(const sync_t *s, uint64_t at);

/* STAMPED: master and flags set (flags may lack health bits); HOLD: anchor too old, retry after the
 * next one; UNKNOWN: no anchor, clock lost, or anchors too far apart or disagreeing. */
#define SYNC_STAMPED 0
#define SYNC_HOLD    1
#define SYNC_UNKNOWN 2
int sync_stamp(const sync_t *s, uint64_t local, int xtal, uint64_t *master, uint8_t *flags);

/* Master time of `local` from the newest anchor (+skew when valid); 0 without an anchor. */
uint64_t sync_to_master(const sync_t *s, uint64_t local);

#endif /* SYNC_H */
