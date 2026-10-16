#include "mac_sensor.h"

#include <string.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "meas.h"
#include "radio.h"
#include "mac_radio.h"
#include "mac_schedule.h"

#define TPM ((uint64_t)TICKS_PER_MS)

/* An uplink let through at SLOT_LATE_MS ends SLOT_GUARD_MS before the LBT listen of the next slot
 * starts, and the last slot ends before the next beacon even when this one went out as late as
 * the absolute beacon channel-access deadline allows. */
static_assert((SLOT_LATE_MS + TX_SETUP_MS + SLOT_GUARD_MS + LBT_LISTEN_MS) * 1000u + LORA_TOA_US(WIRE_UPLINK) <=
              SLOT_LEN_MS * 1000u, "an uplink must fit its slot");
static_assert((SLOT_OFFSET_MS + SLOT_COUNT * SLOT_LEN_MS + BEACON_ACCESS_MS + TX_SETUP_MS) * 1000u +
              LORA_TOA_US(WIRE_ADMISSION) <= BEACON_PERIOD_MS * 1000u, "the admission frame fits");
static_assert((SLOT_LATE_MS + TX_SETUP_MS + SLOT_GUARD_MS + LBT_LISTEN_MS) * 1000u +
              LORA_TOA_US(WIRE_OFFER) <= SLOT_LEN_MS * 1000u, "an offer fits its slot");
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

static int join_authenticated(const sensor_join_t *j)
{
    return j->phase == SENSOR_JOIN_CONFIRM_READY || j->phase == SENSOR_JOIN_WAIT_ASSIGNMENT;
}

static void join_cancel(mac_sensor_t *s)
{
    memset(&s->join, 0, sizeof(s->join));
    s->retry_join_ms = board_millis() + (1u + sec_random() % 4u) * BEACON_PERIOD_MS;
    s->join_retry_pending = 1;
}

static void join_begin(mac_sensor_t *s, uint64_t boot)
{
    if (s->join.phase != SENSOR_JOIN_IDLE || s->join_retry_pending) { return; }
    uint8_t nonce[16];
    if (!sec_challenge(nonce)) { return; }
    memset(&s->join, 0, sizeof(s->join));
    s->join.phase = SENSOR_JOIN_WAIT_OFFER;
    s->join.master_boot = boot;
    s->join.born_ms = board_millis();
    memcpy(s->join.ns, nonce, 16);
}

static int data_entry(const beacon_pl_t *b, const uint8_t schedule[SLOT_COUNT], uint32_t id, uint64_t link)
{
    for (unsigned j = 0; j < SLOT_COUNT; j++) {
        unsigned i = SLOT_ENTRY(schedule[j]);
        if (SLOT_ROLE(schedule[j]) == SLOT_ROLE_DATA && b->slot[i].node_id == id && b->slot[i].link_id == link) {
            return (int)i;
        }
    }
    return -1;
}

static void on_offer(mac_sensor_t *s, const uint8_t *buf, int n)
{
    sensor_join_t *j = &s->join;
    if (j->phase == SENSOR_JOIN_IDLE) { return; }
    offer_pl_t p; sec_meta_t meta;
    if (sec_unseal(buf, n, &meta, &p, sizeof(p)) || meta.boot_id != j->master_boot ||
        p.node_id != s->my_id || p.sensor_boot != sec_boot_id() || !p.link_id ||
        memcmp(p.ns, j->ns, 16) || meta.ctr <= j->offer_ctr) { return; }
    uint32_t floor = (uint32_t)p.beacon_ctr[0] | (uint32_t)p.beacon_ctr[1] << 8 | (uint32_t)p.beacon_ctr[2] << 16;
    if (!floor || floor >= meta.ctr || floor < j->beacon_floor) { return; }
    if (join_authenticated(j) && (j->link_id != p.link_id || memcmp(j->nm, p.nm, 16))) { return; }
    if (!join_authenticated(j)) {
        if (!sec_link_keys(&j->keys, j->master_boot, s->my_id, sec_boot_id(), j->ns, p.nm, p.link_id)) { return; }
        memcpy(j->nm, p.nm, 16); j->link_id = p.link_id;
        j->phase = SENSOR_JOIN_CONFIRM_READY;
        sync_init(&j->sync);
        /* Only the exact advertised beacon may seed this candidate. Its first
         * RxDone was cached before the offer; duplicate packets never replace it. */
        if (s->hint_valid && s->hint_boot == j->master_boot && s->hint_ctr == floor) {
            (void)sync_beacon(&j->sync, s->hint.base.seq, s->hint_rx, 1, 0, 0);
            j->seen_ctr = floor;
        }
    }
    s->retry.at_ms = board_millis() + BEACON_LOSS_RESET_MS;
    j->offer_ctr = meta.ctr;
    j->beacon_floor = floor;
}

static int beacon_schedule(const admission_beacon_t *b, uint8_t slots[SLOT_COUNT])
{
    if (b->base.flags & BEACON_ADMISSION) { memcpy(slots, b->schedule, SLOT_COUNT); }
    else { mac_normal_schedule(&b->base, slots); }
    if (!mac_schedule_valid(slots)) { return 0; }
    for (unsigned k = 0; k < SLOT_COUNT; k++) {
        unsigned role = SLOT_ROLE(slots[k]), i = SLOT_ENTRY(slots[k]);
        if (role != SLOT_ROLE_IDLE && role != SLOT_ROLE_REQUEST &&
            (!b->base.slot[i].node_id || !b->base.slot[i].link_id)) { return 0; }
    }
    return 1;
}

/* Same-clock reauthentication keeps FIFO, sync and capture identity. Only an
 * authenticated change of master epoch clears records tied to the old clock. */
static void activate_join(mac_sensor_t *s, int entry, uint32_t ctr)
{
    sensor_join_t *j = &s->join;
    if (!s->have_session || s->master_boot_id != j->master_boot) {
        sq_clear(&s->q);
        s->sync = j->sync;
        s->timing_ready = 0;
        s->cp_pending = s->cp_answered = 0;
    }
    s->master_boot_id = j->master_boot; s->link_id = j->link_id;
    memcpy(s->data_key, j->keys.data, 32);
    s->have_session = 1; s->entry = entry;
    memset(&s->from_master, 0, sizeof(s->from_master));
    s->from_master.max_ctr = ctr;
    s->last_beacon_ms = board_millis();
    memset(j, 0, sizeof(*j));
}

/* Returns 1 only when the candidate becomes the active connection. */
static int join_beacon(mac_sensor_t *s, const admission_beacon_t *b, const sec_meta_t *meta,
                       const uint8_t slots[SLOT_COUNT], uint64_t l_rx)
{
    sensor_join_t *j = &s->join;
    if (!join_authenticated(j) || j->master_boot != meta->boot_id || meta->ctr <= j->beacon_floor) { return 0; }
    if (sec_accept_counter(&j->seen_ctr, meta->ctr)) {
        if (j->sync.have_prev && l_rx - j->sync.prev_lrx > 255u * BEACON_PERIOD_MS * TPM) { sync_clock_lost(&j->sync); }
        (void)sync_beacon(&j->sync, b->base.seq, l_rx, 1, b->base.flags & BEACON_TX_PREV_VALID, b->base.m_tx_prev);
    }
    int entry = data_entry(&b->base, slots, s->my_id, j->link_id);
    if (j->phase == SENSOR_JOIN_WAIT_ASSIGNMENT && entry >= 0) {
        activate_join(s, entry, meta->ctr);
        return 1;
    }
    for (unsigned i = 0; i < MAX_NODES; i++) {
        if ((b->pending & (1u << i)) && b->base.slot[i].node_id == s->my_id) { return 0; }
    }
    join_cancel(s);
    return 0;
}

static void select_tx_slots(mac_sensor_t *s, const admission_beacon_t *b,
                            const uint8_t slots[SLOT_COUNT], int active)
{
    s->tx_slots = 0; s->data_sent = 0;
    unsigned requests[SLOT_COUNT], nrequests = 0;
    for (unsigned k = 0; k < SLOT_COUNT; k++) {
        unsigned role = SLOT_ROLE(slots[k]), i = SLOT_ENTRY(slots[k]);
        if (role == SLOT_ROLE_DATA && active && (int)i == s->entry) { s->tx_slots |= (uint8_t)(1u << k); }
        if (role == SLOT_ROLE_REQUEST) { requests[nrequests++] = k; }
        if (role == SLOT_ROLE_CONFIRM && s->join.phase != SENSOR_JOIN_IDLE && b->base.slot[i].node_id == s->my_id) {
            s->tx_slots |= (uint8_t)(1u << k);
        }
    }
    if (s->join.phase == SENSOR_JOIN_WAIT_OFFER && nrequests &&
        (nrequests > 1u || (sec_random() & 1u))) {
        s->tx_slots |= (uint8_t)(1u << requests[sec_random() % nrequests]);
    }
}

static void on_beacon(mac_sensor_t *s, const uint8_t *buf, int n)
{
    uint64_t l_rx;
    if (!capture_dio1_get(&l_rx)) { return; }
    admission_beacon_t b = {0}; sec_meta_t meta;
    int plen = n == WIRE_ADMISSION ? sizeof(b) : sizeof(b.base);
    if (sec_unseal(buf, n, &meta, &b, plen) || meta.node_id != NODE_MASTER ||
        !!(b.base.flags & BEACON_ADMISSION) != (n == WIRE_ADMISSION) ||
        b.base.flags & ~(BEACON_ADMISSION | BEACON_TX_PREV_VALID) || b.pending & ~0x1fu) { return; }
    uint8_t slots[SLOT_COUNT];
    if (!beacon_schedule(&b, slots)) { return; }
    int active = s->have_session && meta.boot_id == s->master_boot_id;
    if (active) {
        if (!sec_accept_counter(&s->from_master.max_ctr, meta.ctr)) { return; }
        s->last_beacon_ms = board_millis();
        if (s->join.phase != SENSOR_JOIN_IDLE && s->join.master_boot != s->master_boot_id) { join_cancel(s); }
        if (s->sync.have_prev && l_rx - s->sync.prev_lrx > 255u * BEACON_PERIOD_MS * TPM) { sync_clock_lost(&s->sync); }
        (void)sync_beacon(&s->sync, b.base.seq, l_rx, 1, b.base.flags & BEACON_TX_PREV_VALID, b.base.m_tx_prev);
        sq_resolve(&s->q, &s->sync, capture_now64());
        s->entry = data_entry(&b.base, slots, s->my_id, s->link_id);
        if (s->entry < 0) { join_begin(s, meta.boot_id); }
    } else {
        /* A foreign epoch cannot disturb a recently heard active master. */
        if (s->have_session && (uint32_t)(board_millis() - s->last_beacon_ms) < SYNC_TTL_MS) { return; }
        join_begin(s, meta.boot_id);
        if (s->join.phase == SENSOR_JOIN_IDLE || s->join.master_boot != meta.boot_id) { return; }
    }
    if (s->hint_valid && s->hint_boot == meta.boot_id && meta.ctr <= s->hint_ctr) { return; }
    s->hint = b; s->hint_boot = meta.boot_id; s->hint_ctr = meta.ctr; s->hint_rx = l_rx; s->hint_valid = 1;
    if (join_beacon(s, &b, &meta, slots, l_rx)) { active = 1; }
    if (active && s->entry >= 0) {
        sq_ack(&s->q, b.base.slot[s->entry].ack_seq);
        if (b.base.cp_req && b.base.cp_req != s->cp_answered) { s->cp_pending = b.base.cp_req; }
        sq_resolve(&s->q, &s->sync, capture_now64());
    }
    select_tx_slots(s, &b, slots, active);
    memcpy(s->schedule, slots, sizeof(slots));
    s->cycle_rx = l_rx; s->cycle_seq = b.base.seq;
    s->retry.delay_ms = BEACON_LOSS_RESET_MS;
    board_led_toggle();
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
    if (s->have_session && !s->timing_ready) { return; }
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
    if (!s->tx_slots) { return; }
    unsigned j = 0;
    while (!(s->tx_slots & (1u << j))) { j++; }
    uint64_t start = s->cycle_rx + (SLOT_OFFSET_MS + j * SLOT_LEN_MS) * TPM;
    uint64_t late = start + SLOT_LATE_MS * TPM;
    uint64_t listen = radio_lbt_enabled() ? LBT_LISTEN_MS * TPM : 0;
    if (capture_now64() + listen < start) { return; }
    s->tx_slots &= (uint8_t)~(1u << j);
    if (capture_now64() > late) { return; }
    unsigned role = SLOT_ROLE(s->schedule[j]);
    if (role == SLOT_ROLE_DATA && (s->data_sent || !s->have_session || s->entry < 0)) { return; }
    if (role == SLOT_ROLE_CONFIRM && (!join_authenticated(&s->join) ||
        s->hint_boot != s->join.master_boot || s->hint_ctr < s->join.beacon_floor)) { return; }
    if (role == SLOT_ROLE_REQUEST && s->join.phase != SENSOR_JOIN_WAIT_OFFER) { return; }
    mac_radio_limit(late);
    if (listen && radio_lbt_listen() != RADIO_LBT_QUIET) { radio_unlimit(); return; }
    drain_captures(s);
    uplink_pl_t *p = NULL;
    if (role == SLOT_ROLE_DATA) {
        p = sq_packet(&s->q, s->master_boot_id);
        if (!p && checkpoint_due(s)) { p = make_checkpoint(s); }
        if (!p) { radio_unlimit(); return; }
    }
    if (radio_rx_settle(0, 0) != RADIO_RX_IDLE || radio_cad() != RADIO_CAD_CLEAR) { radio_unlimit(); return; }
    uint8_t tx[WIRE_MAX]; int n = -1;
    if (role == SLOT_ROLE_DATA) {
        fill_diag(s, p);
        n = sec_seal_key(s->data_key, tx, sizeof(tx), PKT_TYPE_UPLINK, s->my_id, p, sizeof(*p));
    } else if (role == SLOT_ROLE_REQUEST) {
        join_pl_t r = {.master_boot = s->join.master_boot}; memcpy(r.ns, s->join.ns, 16);
        n = sec_seal(tx, sizeof(tx), PKT_TYPE_JOIN, s->my_id, &r, sizeof(r));
    } else if (role == SLOT_ROLE_CONFIRM) {
        confirm_pl_t c = {.master_boot = s->join.master_boot}; memcpy(c.nm, s->join.nm, 16);
        n = sec_seal_key(s->join.keys.confirm, tx, sizeof(tx), PKT_TYPE_CONFIRM, s->my_id, &c, sizeof(c));
    }
    uint64_t unused; (void)capture_dio1_get(&unused);
    uint64_t finish = start + (SLOT_LEN_MS - SLOT_GUARD_MS - LBT_LISTEN_MS) * TPM;
    if (n > 0 && capture_now64() <= late && radio_tx_start(tx, n, mac_deadline_us(finish)) == 0) {
        s->tx_busy = 1;
        if (role == SLOT_ROLE_DATA) { s->data_sent = 1; }
        if (role == SLOT_ROLE_CONFIRM) { s->join.phase = SENSOR_JOIN_WAIT_ASSIGNMENT; }
        return;
    }
    if (!radio_needs_reset()) { radio_start_rx(); }
    radio_unlimit();
}

static void radio_recovery(mac_sensor_t *s)
{
    uint32_t now = board_millis();
    int starving = (uint32_t)(now - s->last_beacon_ms) > BEACON_LOSS_RESET_MS;
    if (!starving && !radio_needs_reset()) { return; }
    if (!retry_due(&s->retry, now)) { return; }
    el_radio_recovery_begin(radio_needs_reset() ? el_radio_recovery_reason() : RADIO_RECOVERY_BEACON_LOSS);
    int ok = radio_begin() == 0 && radio_start_rx() == 0;
    el_radio_recovery_end(ok);
    s->tx_slots = 0;
    retry_later(&s->retry, board_millis(), RADIO_RESET_BACKOFF_MAX_MS);
}

void mac_sensor_step(mac_sensor_t *s)
{
    (void)capture_now64(); /* at least once per TIMER1 wrap, beacons or not (capture.h) */
    /* Retire an elapsed backoff even while no beacon asks us to join. An old
     * deadline must never turn into a future wait after half a millis period. */
    if (s->join_retry_pending && (int32_t)(board_millis() - s->retry_join_ms) >= 0) {
        s->join_retry_pending = 0;
    }
    if (s->tx_busy) {
        int done = radio_tx_poll();
        if (!done) { drain_captures(s); return; }
        s->tx_busy = 0;
        uint64_t unused; (void)capture_dio1_get(&unused);
    }
    if (s->join.phase != SENSOR_JOIN_IDLE && (uint32_t)(board_millis() - s->join.born_ms) >= JOIN_LIFETIME_MS) { join_cancel(s); }
    uint8_t buf[WIRE_MAX];
    radio_limit(board_micros() + 7000u);
    int n = radio_needs_reset() ? 0 : radio_receive(buf, sizeof(buf), NULL, NULL);
    radio_unlimit();
    if (n >= WIRE_BEACON && SEC_VT_TYPE(buf[0]) == PKT_TYPE_BEACON) { on_beacon(s, buf, n); }
    else if (n == WIRE_OFFER && SEC_VT_TYPE(buf[0]) == PKT_TYPE_OFFER) { on_offer(s, buf, n); }

    uint32_t stops = board_hfxo_stops();
    if (!board_hfclk_xtal() || stops != s->hfxo_stops) {
        /* A recovered oscillator needs fresh anchors: the old skew cannot
         * certify captures across the clock-source change, even when the
         * restart finished before this check. */
        s->hfxo_stops = stops;
        sync_clock_lost(&s->sync);
        sync_clock_lost(&s->join.sync);
    }
    if (s->have_session && !s->timing_ready) {
        uint64_t now = capture_now64();
        if (sync_health(&s->sync, now, board_hfclk_xtal()) == HEALTH_EVENT_REQUIRED) {
            s->timing_ready = 1;
            s->timing_ready_tick = now;
        }
    }
    drain_captures(s);
    if (radio_needs_reset()) { s->tx_slots = 0; }
    else { slot_tx(s); }
    if (!s->tx_busy) {
        uint64_t end = s->cycle_rx + (SLOT_OFFSET_MS + SLOT_COUNT * SLOT_LEN_MS) * TPM;
        if (!s->tx_slots && capture_now64() >= end) { meas_service(board_millis()); radio_recovery(s); }
    }
}
