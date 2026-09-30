#include "mac_sensor.h"

#include <string.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "meas.h"
#include "radio.h"

#define TPM ((uint64_t)TICKS_PER_MS)

void mac_sensor_init(mac_sensor_t *s, uint32_t my_id, uint8_t reset_reason)
{
    memset(s, 0, sizeof(*s));
    s->my_id = my_id;
    s->short_id = node_short_id(my_id);
    s->boot_tag = (uint8_t)sec_boot_id();
    s->reset_reason = reset_reason;
    sync_init(&s->sync);
    sq_init(&s->q);
    /* Start ev_seq at a boot-random value: a stale slot entry left by our previous
     * boot must match both the 8-bit boot tag and this 16-bit seq to be taken as
     * an ACK. */
    s->q.next_ev_seq = (uint16_t)(sec_boot_id() >> 16);
    if (s->q.next_ev_seq == 0u) { s->q.next_ev_seq = 1u; }
    s->slot = -1;
    s->tx_slot = -1;
    s->last_beacon_ms = board_millis();
    s->reset_after_ms = s->last_beacon_ms;
    s->reset_backoff_ms = BEACON_LOSS_RESET_MS;
    s->hfxo_stops = board_hfxo_stops();
    radio_start_rx();
}

/* Unregistered: contend in a random slot of `mask` with probability 1/2. */
static int contend_slot(unsigned mask)
{
    unsigned n = 0;
    for (unsigned k = 0; k < MAX_NODES; k++) { n += (mask >> k) & 1u; }
    uint32_t r = sec_random();
    if (n == 0 || (r & 1u)) { return -1; }
    unsigned pick = (r >> 1) % n;
    for (unsigned k = 0; k < MAX_NODES; k++) {
        if (((mask >> k) & 1u) && pick-- == 0) { return (int)k; }
    }
    return -1;
}

static void on_beacon(mac_sensor_t *s, const uint8_t *buf, int n)
{
    uint64_t l_rx = 0;
    int l_rx_ok = capture_dio1_get(&l_rx);
    sec_meta_t m;
    beacon_pl_t b;
    if (sec_unseal(buf, n, &m, &b, sizeof(b)) != 0 || m.node_id != NODE_MASTER) { return; }
    if (!sec_replay(&s->from_master, m.boot_id, m.ctr)) { return; }
    board_led_toggle();
    s->last_beacon_ms = board_millis();
    s->reset_backoff_ms = BEACON_LOSS_RESET_MS;

    if (!s->have_session || m.boot_id != s->master_boot_id) {
        /* New master session (or first contact): the old timebase is gone and
         * the new master refuses records bound to it. */
        s->have_session = 1;
        s->master_boot_id = m.boot_id;
        sync_session(&s->sync);
        sq_clear(&s->q);
        s->timing_ready = 0;
        s->cp_pending = s->cp_answered = 0;
    }
    /* Held captures get stamped by a new anchor, or become losses once their
     * anchor is too old for any later one to bracket them. */
    (void)sync_beacon(&s->sync, b.seq, l_rx, l_rx_ok, (b.flags & BEACON_TX_PREV_VALID) != 0, b.m_tx_prev);
    sq_resolve(&s->q, &s->sync, capture_now64());
    if (b.cp_req != 0u && b.cp_req != s->cp_answered) { s->cp_pending = b.cp_req; }

    /* Our slot carries our short id and this boot's tag. The same short id under
     * another tag is our entry from before a reboot, or a sensor whose id shares
     * our low 16 bits: contend in a free slot rather than jam that one, and use it
     * only when no slot is free (the master then updates the tag). */
    unsigned free_mask = 0, named_mask = 0;
    s->slot = -1;
    for (unsigned k = 0; k < MAX_NODES; k++) {
        if (b.slot[k].short_id == 0u) {
            free_mask |= 1u << k;
        } else if (b.slot[k].short_id == s->short_id) {
            if (b.slot[k].boot_tag == s->boot_tag) {
                s->slot = (int)k;
                sq_ack(&s->q, b.slot[k].ack_seq);
            } else {
                named_mask |= 1u << k;
            }
        }
    }
    /* Without our RxDone the slot times are unknown: stay silent this cycle. */
    s->cycle = l_rx_ok;
    s->cycle_rx = l_rx;
    s->cycle_seq = b.seq;
    s->tx_slot = s->slot >= 0 ? s->slot : contend_slot(free_mask ? free_mask : named_mask);
}

static void record_capture(mac_sensor_t *s, uint32_t seq, uint64_t tick, int xtal)
{
    if (!s->timing_ready || tick < s->timing_ready_tick) { return; } /* before this session's sync */
    uint64_t master;
    uint8_t flags;
    int r = sync_stamp(&s->sync, tick, xtal, &master, &flags);
    if (r == SYNC_STAMPED && (flags & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED) {
        sq_push_edge(&s->q, seq, master, flags, sync_age_ms(&s->sync, tick));
    } else if (r == SYNC_HOLD) {
        sq_push_held(&s->q, seq, tick);
    } else {
        sq_push_loss(&s->q, seq, seq, 0, 0, EVENT_TIME_UNKNOWN);
    }
}

static void record_loss(mac_sensor_t *s, uint32_t first_seq, uint32_t last_seq,
                        uint64_t first_tick, uint64_t last_tick)
{
    if (!s->timing_ready) { return; }
    int xtal = board_hfclk_xtal();
    uint64_t m1, m2;
    uint8_t f1, f2;
    if (sync_stamp(&s->sync, first_tick, xtal, &m1, &f1) == SYNC_STAMPED &&
        sync_stamp(&s->sync, last_tick, xtal, &m2, &f2) == SYNC_STAMPED &&
        ((f1 & f2) & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED) {
        sq_push_loss(&s->q, first_seq, last_seq, m1, m2, (uint8_t)(f1 & f2));
    } else {
        sq_push_loss(&s->q, first_seq, last_seq, 0, 0, EVENT_TIME_UNKNOWN);
    }
}

static void drain_captures(mac_sensor_t *s)
{
    for (;;) {
        uint64_t tick;
        uint32_t seq;
        int xtal;
        if (capture_sensor_get(&tick, &seq, &xtal)) { record_capture(s, seq, tick, xtal); continue; }
        uint64_t t1, t2;
        uint32_t q1, q2;
        if (capture_sensor_loss(&t1, &t2, &q1, &q2)) { record_loss(s, q1, q2, t1, t2); continue; }
        return;
    }
}

static int checkpoint_due(const mac_sensor_t *s, int k)
{
    if (s->cp_pending || s->slot < 0) { return 1; } /* request, or announce ourselves */
    return (uint8_t)(s->cycle_seq % CHECKPOINT_PERIOD_BEACONS) == (uint8_t)((unsigned)k % CHECKPOINT_PERIOD_BEACONS);
}

/* "Through now, the last capture is seq": only when nothing precedes it. */
static uplink_pl_t *make_checkpoint(mac_sensor_t *s)
{
    uint64_t at;
    uint32_t seq;
    if (!sq_idle(&s->q) || !capture_sensor_checkpoint(&at, &seq)) { return NULL; }
    uint8_t flags = sync_health(&s->sync, at, board_hfclk_xtal());
    uint64_t master = 0;
    if (s->sync.cur.have) { master = sync_to_master(&s->sync, at); }
    else { flags |= EVENT_TIME_UNKNOWN; }
    uplink_pl_t *p = sq_checkpoint(&s->q, s->master_boot_id, seq, master, flags, sync_age_ms(&s->sync, at));
    if (p && s->cp_pending) {
        s->cp_answered = s->cp_pending;
        s->cp_pending = 0;
    }
    return p;
}

/* Diagnostics ride every transmission (the evidence part of an in-flight
 * packet never changes). */
static void fill_diag(mac_sensor_t *s, uplink_pl_t *p)
{
    uint64_t now = capture_now64();
    ul_diag_t *d = &p->diag;
    d->health = sync_health(&s->sync, now, board_hfclk_xtal());
    d->sync_age_ms = sync_age_ms(&s->sync, now);
    d->skew_ppm = (int16_t)s->sync.skew_ppm;
    d->rx_miss = s->sync.rx_miss;
    d->beacon_gap = s->sync.beacon_gap;
    d->batt_mv = (uint16_t)(meas_vddh_mv() + BATT_DIODE_DROP_MV);
    d->temp_c10 = meas_temp_c10();
    d->capture_overflow = capture_sensor_overflow();
    d->fifo_drop = s->q.fifo_drop;
    d->err_flags = el_flags();
    d->reset_reason = s->reset_reason;
}

static void slot_tx(mac_sensor_t *s)
{
    if (!s->cycle) { return; }
    if (s->tx_slot < 0) { s->cycle = 0; return; }
    uint64_t start = s->cycle_rx + (uint64_t)(SLOT_OFFSET_MS + (unsigned)s->tx_slot * SLOT_LEN_MS) * TPM;
    uint64_t now = capture_now64();
    if (now < start) { return; }
    s->cycle = 0; /* one attempt per cycle */
    if (now - start > SLOT_LATE_MS * TPM) { return; }

    uplink_pl_t *p = sq_packet(&s->q, s->master_boot_id);
    if (!p && checkpoint_due(s, s->tx_slot)) { p = make_checkpoint(s); }
    if (!p) { return; }

    /* Listen before talk. The slot is ours, so anything on air is a stranger:
     * give the slot up rather than wait. A bare preamble detection (maybe a stale
     * one) is left to the CAD. */
    if (radio_rx_settle(0, 0) != RADIO_RX_IDLE) { return; }
    if (radio_cad()) { return; }
    if (capture_now64() - start > SLOT_LATE_MS * TPM) { radio_start_rx(); return; }

    fill_diag(s, p);
    uint8_t tx[WIRE_UPLINK];
    int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_UPLINK, s->my_id, p, sizeof(*p));
    if (wlen > 0) {
        uint64_t tx_done;
        radio_transmit(tx, wlen);
        (void)capture_dio1_get(&tx_done); /* a TxDone must not pass for the next beacon's RxDone */
    }
    radio_start_rx();
}

static void radio_recovery(mac_sensor_t *s)
{
    uint32_t now = board_millis();
    int starving = (uint32_t)(now - s->last_beacon_ms) > BEACON_LOSS_RESET_MS;
    if (!starving && !radio_needs_reset()) { return; }
    if ((int32_t)(now - s->reset_after_ms) < 0) { return; }
    el_note(EL_RADIO_RESET);
    radio_begin();
    radio_start_rx();
    s->cycle = 0;
    s->reset_after_ms = now + s->reset_backoff_ms;
    s->reset_backoff_ms = s->reset_backoff_ms * 2u > RADIO_RESET_BACKOFF_MAX_MS
                          ? RADIO_RESET_BACKOFF_MAX_MS : s->reset_backoff_ms * 2u;
}

void mac_sensor_step(mac_sensor_t *s)
{
    (void)capture_now64(); /* at least once per TIMER1 wrap, beacons or not (capture.h) */
    uint8_t buf[WIRE_MAX];
    int n = radio_receive(buf, sizeof(buf), NULL, NULL);
    if (n >= WIRE_BEACON && SEC_VT_TYPE(buf[0]) == PKT_TYPE_BEACON) { on_beacon(s, buf, n); }

    uint32_t stops = board_hfxo_stops();
    if (!board_hfclk_xtal() || stops != s->hfxo_stops) {
        /* A recovered oscillator needs fresh anchors: the old skew cannot
         * certify captures across the clock-source change, even when the
         * restart finished before this check. */
        s->hfxo_stops = stops;
        sync_clock_lost(&s->sync);
    }
    if (!s->timing_ready) {
        uint64_t now = capture_now64();
        if (sync_health(&s->sync, now, board_hfclk_xtal()) == HEALTH_EVENT_REQUIRED) {
            s->timing_ready = 1;
            s->timing_ready_tick = now;
        }
    }
    drain_captures(s);
    slot_tx(s);
    radio_recovery(s);
}
