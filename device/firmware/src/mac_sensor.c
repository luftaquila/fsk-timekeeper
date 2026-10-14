#include "mac_sensor.h"

#include <string.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "meas.h"
#include "radio.h"

#define TPM ((uint64_t)TICKS_PER_MS)

/* An uplink let through at SLOT_LATE_MS ends SLOT_GUARD_MS before the LBT listen of the next slot
 * starts, and the last slot ends before the next beacon even when this one went out as late as
 * the master's LBT allows (beacon_lbt(): re-sensing, a last CAD, then a drain and a listen). */
static_assert((SLOT_LATE_MS + TX_SETUP_MS + SLOT_GUARD_MS + LBT_LISTEN_MS) * 1000u + LORA_TOA_US(WIRE_UPLINK) <=
              SLOT_LEN_MS * 1000u, "an uplink must fit its slot");
static_assert((SLOT_OFFSET_MS + SLOT_COUNT * SLOT_LEN_MS + BEACON_LBT_MAX_MS + CAD_TIMEOUT_MS + RX_DRAIN_MAX_MS +
               LBT_LISTEN_MS + TX_SETUP_MS) * 1000u + LORA_TOA_US(WIRE_BEACON) <= BEACON_PERIOD_MS * 1000u,
              "the slots must end before the next beacon");
static_assert(LBT_LISTEN_MS < SLOT_OFFSET_MS, "the listen before slot 0 starts after the beacon");
static_assert(SLOT_COUNT >= 2u && SLOT_COUNT <= 8u, "tx_slots holds every slot in 8 bits");
static_assert(LORA_SF >= 7 && LORA_SYMBOL_US < 16000u, "LORA_TOA_US covers SF7..SF12 without LDRO");

void mac_sensor_init(mac_sensor_t *s, uint32_t my_id, uint8_t reset_reason)
{
    memset(s, 0, sizeof(*s));
    s->my_id = my_id;
    s->reset_reason = reset_reason;
    sync_init(&s->sync);
    sq_init(&s->q);
    s->entry = -1;
    s->last_beacon_ms = board_millis();
    retry_init(&s->retry, s->last_beacon_ms, BEACON_LOSS_RESET_MS);
    s->hfxo_stops = board_hfxo_stops();
    radio_start_rx();
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
    s->retry.delay_ms = BEACON_LOSS_RESET_MS;

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

    /* Match the complete identity before applying an ACK or claiming slots.
     * Unregistered sensors contend only in the last slot. */
    unsigned live = 0, rank = 0;
    s->entry = -1;
    for (unsigned k = 0; k < MAX_NODES; k++) {
        if (b.slot[k].node_id == 0u) { continue; }
        if (b.slot[k].node_id == s->my_id && b.slot[k].boot_id == sec_boot_id()) {
            s->entry = (int)k;
            rank = live;
            sq_ack(&s->q, b.slot[k].ack_seq);
        }
        live++;
    }
    /* Without our RxDone the slot times are unknown: stay silent this cycle. */
    s->tx_slots = 0;
    if (l_rx_ok && s->entry >= 0) {
        for (unsigned j = rank; j < SLOT_COUNT - 1u; j += live) { s->tx_slots |= (uint8_t)(1u << j); }
    } else if (l_rx_ok && (sec_random() & 1u)) {
        s->tx_slots = (uint8_t)(1u << (SLOT_COUNT - 1u));
    }
    s->cycle_rx = l_rx;
    s->cycle_seq = b.seq;
    /* At least SLOT_OFFSET_MS before the first slot, subject to the final deadline check. */
    meas_service(board_millis());
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
        int loss = capture_sensor_loss(&t1, &t2, &q1, &q2);
        if (loss) {
            if (loss == CAPTURE_LOSS_UNKNOWN) {
                if (s->timing_ready) { sq_push_loss(&s->q, q1, q2, 0, 0, EVENT_TIME_UNKNOWN); }
            } else { record_loss(s, q1, q2, t1, t2); }
            continue;
        }
        return;
    }
}

/* On request, or to announce ourselves, at once. The periodic one waits for our last slot of its
 * cycle: one packet per cycle, so an earlier one would hold that cycle's captures back a beacon. */
static int checkpoint_due(const mac_sensor_t *s)
{
    if (s->cp_pending || s->entry < 0) { return 1; }
    return s->tx_slots == 0u && (uint8_t)(s->cycle_seq % CHECKPOINT_PERIOD_BEACONS) ==
                                (uint8_t)((unsigned)s->entry % CHECKPOINT_PERIOD_BEACONS);
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
    if (s->tx_slots == 0u) { return; }
    unsigned j = 0;
    while (!((s->tx_slots >> j) & 1u)) { j++; }
    uint64_t start = s->cycle_rx + (uint64_t)(SLOT_OFFSET_MS + j * SLOT_LEN_MS) * TPM;
    uint64_t late = start + SLOT_LATE_MS * TPM;
    /* The RSSI listen takes the LBT_LISTEN_MS before the slot, so the slot keeps its time. */
    uint64_t listen = radio_lbt_enabled() ? LBT_LISTEN_MS * TPM : 0u;
    uint64_t now = capture_now64();
    if (now + listen < start) { return; }
    s->tx_slots &= (uint8_t)~(1u << j); /* one try per slot */
    if (now > late) { return; }

    /* Listen before talk. The slot is ours, so anything on air is a stranger:
     * give the slot up rather than wait. The listen comes before the packet is
     * built, so captures taken during it still ride along. A bare preamble
     * detection (maybe a stale one) is left to the CAD. */
    if (listen) {
        if (radio_lbt_listen() != RADIO_LBT_QUIET) { return; }
        drain_captures(s);
    }
    uplink_pl_t *p = sq_packet(&s->q, s->master_boot_id);
    if (!p && checkpoint_due(s)) { p = make_checkpoint(s); }
    if (!p) { return; }

    if (radio_rx_settle(0, 0) != RADIO_RX_IDLE) { return; }
    if (radio_cad()) { return; }
    if (capture_now64() > late) { radio_start_rx(); return; }

    fill_diag(s, p);
    uint8_t tx[WIRE_UPLINK];
    int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_UPLINK, s->my_id, p, sizeof(*p));
    if (wlen > 0 && capture_now64() <= late) {
        s->tx_slots = 0; /* one packet per cycle; the next beacon acks it */
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
    if (!retry_due(&s->retry, now)) { return; }
    el_note(EL_RADIO_RESET);
    if (radio_begin() == 0) { radio_start_rx(); }
    s->tx_slots = 0;
    retry_later(&s->retry, board_millis(), RADIO_RESET_BACKOFF_MAX_MS);
}

void mac_sensor_step(mac_sensor_t *s)
{
    (void)capture_now64(); /* at least once per TIMER1 wrap, beacons or not (capture.h) */
    uint8_t buf[WIRE_MAX];
    int n = radio_needs_reset() ? 0 : radio_receive(buf, sizeof(buf), NULL, NULL);
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
    if (radio_needs_reset()) { s->tx_slots = 0; }
    else { slot_tx(s); }
    radio_recovery(s);
}
