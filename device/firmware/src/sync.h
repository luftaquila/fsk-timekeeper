/* Sensor-side time sync to the master timebase (DESIGN §2.4, §2.5). Pure logic.
 *
 * Beacon N carries the master TxDone tick of beacon N-1; paired with the
 * sensor's RxDone of N-1 it measures frequency, not absolute offset. A selected
 * uplink supplies the other direction: the midpoint of local RxDone/TxDone is
 * paired with the midpoint of master TxDone/RxDone. Symmetric radio delay cancels;
 * path asymmetry leaves half the directional difference. No fixed delay is used.
 * A capture is stamped
 * immediately from the newest anchor while that anchor is fresh; a capture taken
 * while it is stale is held and stamped by interpolation once the next anchor
 * exists.
 */
#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

#define SYNC_HIST 8u /* anchors kept for the skew estimate */

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
    uint16_t rx_miss;       /* beacons missed since boot (saturating) */
    uint8_t  beacon_gap;    /* beacons missed in a row right now */
} sync_t;

/* Boot: everything zero. */
void sync_init(sync_t *s);

/* HFXO not running: anchors and skew no longer describe the local clock. */
void sync_clock_lost(sync_t *s);

/* Account one authenticated beacon of the current session. l_rx_ok = its RxDone
 * was captured; m_tx_ok = m_tx_prev is the TxDone of beacon seq-1. Returns 1
 * when the beacon produced a frequency sample. air_class identifies this
 * beacon's packet length; only equal-length endpoints estimate frequency. */
int sync_beacon(sync_t *s, uint8_t seq, uint64_t l_rx, int l_rx_ok, int m_tx_ok, uint64_t m_tx_prev,
                uint8_t air_class);

/* The MAC authenticates and matches all four captures to the same frame/link,
 * with identical packet lengths in both directions. Returns 1 for a new anchor. */
int sync_roundtrip(sync_t *s, uint64_t m_tx, uint64_t l_rx, uint64_t l_tx, uint64_t m_rx);

/* Health bits (HEALTH_*) of a capture at local tick `at` with HFXO state xtal. */
uint8_t sync_health(const sync_t *s, uint64_t at, int xtal);

/* Age of the newest anchor at local tick `at`, ms, saturated (UINT16_MAX = none). */
uint16_t sync_age_ms(const sync_t *s, uint64_t at);

/* Stamp a capture at local tick `local` with HFXO state xtal: SYNC_STAMPED (master tick and flags
 * set, flags may lack health bits), SYNC_HOLD (anchor too old: keep the local tick, retry after
 * the next anchor), SYNC_UNKNOWN (no anchor, clock lost, anchors too far apart or disagreeing). */
#define SYNC_STAMPED 0
#define SYNC_HOLD    1
#define SYNC_UNKNOWN 2
int sync_stamp(const sync_t *s, uint64_t local, int xtal, uint64_t *master, uint8_t *flags);

/* Master time of `local` from the newest anchor (with skew when valid), for
 * checkpoints and loss ranges; 0 if there is no anchor. */
uint64_t sync_to_master(const sync_t *s, uint64_t local);

#endif /* SYNC_H */
