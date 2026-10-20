#include "mac_master.h"

#include <string.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "proto_usb.h"
#include "radio.h"
#include "meas.h"
#include "secure.h"
#include "mac_radio.h"

#define TPM ((uint64_t)TICKS_PER_MS)
#define BEACON_PERIOD_TICKS ((uint64_t)BEACON_PERIOD_MS * TPM)

void mac_master_init(mac_master_t *m, mq_t *q)
{
    memset(m, 0, sizeof(*m));
    reg_init(&m->reg);
    m->q = q;
    m->next_beacon = capture_now64();
    m->last_report_ms = board_millis();
    retry_init(&m->retry, board_millis(), RADIO_BOOT_RETRY_MS);
    master_admission_session(&m->admission, board_millis());
    radio_start_rx();
}

void mac_master_checkpoint(mac_master_t *m)
{
    m->cp_id = (uint8_t)(m->cp_id % 255u + 1u); /* 1..255, never the "no request" 0 */
    m->cp_left = CP_REQ_BEACONS;
}

void mac_master_session(mac_master_t *m)
{
    if (m->tx_state != MASTER_TX_IDLE) { radio_tx_abort(); }
    reg_session(&m->reg);
    m->m_tx_valid = 0;
    m->cycle_ready = 0;
    m->reply_valid = 0;
    m->tx_state = MASTER_TX_IDLE;
    master_admission_session(&m->admission, board_millis());
}

static void emit_sensor(const reg_entry_t *e, uint32_t now_ms)
{
    pu_diag_t d;
    memset(&d, 0, sizeof(d));
    d.node_id = e->id;
    d.state = reg_state(e, now_ms);
    d.skew_ppm = e->diag.skew_ppm;
    d.rx_miss = e->diag.rx_miss;
    d.beacon_gap = e->diag.beacon_gap;
    d.last_seen_ms = now_ms - e->heard_ms;
    d.rssi = e->rssi;
    d.snr = e->snr;
    d.lat_ms = e->lat_ms;
    d.temp_c10 = e->diag.temp_c10;
    d.batt_mv = e->diag.batt_mv;
    d.sec_drop = e->sec_drop;
    d.provisioned = 1; /* it authenticated */
    d.health = e->diag.health;
    d.diag_valid = e->diag_valid;
    uint32_t age = (uint32_t)e->diag.sync_age_ms + now_ms - e->heard_ms;
    d.sync_age_ms = age > UINT16_MAX ? UINT16_MAX : (uint16_t)age;
    if (age > SYNC_TTL_MS) { d.health &= (uint8_t)~HEALTH_SYNC_VALID; }
    d.capture_overflow = e->diag.capture_overflow;
    d.fifo_drop = e->diag.fifo_drop;
    d.err_flags = e->diag.err_flags;
    d.reset_reason = e->diag.reset_reason;
    d.sensor_boot_id = e->sensor_boot;
    d.master_boot_id = sec_boot_id();
    pu_emit_diag(&d);
}

void mac_master_report(mac_master_t *m, int changed_only)
{
    uint32_t now = board_millis();
    if ((uint32_t)(now - m->last_report_ms) >= SENSOR_STATUS_PERIOD_MS) {
        changed_only = 0;
    }
    if (!changed_only) { m->last_report_ms = now; }
    for (unsigned i = 0; i < MAX_NODES; i++) {
        reg_entry_t *e = &m->reg.e[i];
        if (!e->used) { continue; }
        int st = reg_state(e, now);
        if (changed_only && st == e->last_state) { continue; }
        e->last_state = st;
        emit_sensor(e, now);
        pu_emit_join(e->id, st == PU_STATE_OK && (uint32_t)e->diag.sync_age_ms + now - e->heard_ms <= SYNC_TTL_MS && (e->diag.health & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED ? PU_JOIN_READY : PU_JOIN_SYNC);
    }
    if (!changed_only) {
        for (unsigned i = 0; i < MAX_NODES; i++) {
            if (m->admission.pending[i].phase != ADMISSION_EMPTY) { pu_emit_join(m->admission.pending[i].id, PU_JOIN_WAIT); }
        }
    }
}

static void on_admission(mac_master_t *m, const uint8_t *buf, int n)
{
    uint32_t id = 0;
    switch (master_admission_receive(&m->admission, &m->reg, buf, n, board_millis(), &id)) {
    case ADMISSION_WAIT: pu_emit_join(id, PU_JOIN_WAIT); break;
    case ADMISSION_ACCEPTED: {
        const reg_entry_t *e = reg_lookup(&m->reg, id);
        if (e) { emit_sensor(e, board_millis()); }
        pu_emit_join(id, PU_JOIN_SYNC);
        break;
    }
    case ADMISSION_FULL: el_note(EL_REGISTRY_FULL); break;
    case ADMISSION_AUTH_FAILED: m->auth_drop++; break;
    case ADMISSION_IGNORED: break;
    }
}

static void on_uplink(mac_master_t *m, const uint8_t *buf, int n, float rssi, float snr,
                       int have_rx, uint64_t rx_tick)
{
    if (SEC_VT_VER(buf[0]) != PROTO_VER) {
        /* Another version counts only when the fleet key opens it: a foreign
         * LoRa device on our channel and sync word is not an outdated board. */
        if (sec_authentic_any_version(buf, n)) { m->ver_drop++; }
        return;
    }
    if (SEC_VT_TYPE(buf[0]) == PKT_TYPE_JOIN || SEC_VT_TYPE(buf[0]) == PKT_TYPE_CONFIRM) {
        on_admission(m, buf, n); return;
    }
    if (SEC_VT_TYPE(buf[0]) != PKT_TYPE_UPLINK || (n != WIRE_UPLINK && n != WIRE_UPLINK_ADMISSION)) { return; }
    uint32_t id; memcpy(&id, buf + 12, 4);
    reg_entry_t *e = reg_lookup(&m->reg, id);
    if (!e || !e->have_boot || !e->link_id) { m->auth_drop++; return; }
    sec_meta_t meta; uplink_pl_t u;
    if (sec_unseal_key(e->data_key, buf, n, &meta, &u, sizeof(u)) ||
        meta.boot_id != e->sensor_boot || u.master_boot_id != sec_boot_id()) { m->auth_drop++; return; }
    if (!sec_accept_counter(&e->rx.max_ctr, meta.ctr)) { e->sec_drop++; return; }
    /* Refresh the oldest reported anchor first; rotate the entry used to break
     * ties. A fresh anchor must not displace another sensor's due refresh.
     * Diagnostics-only follow-ups have fresh anchors and therefore do not
     * displace ordinary refreshes. A sensor without its crystal discards every
     * anchor (mac_sensor.c), so it is no candidate: it would keep the oldest age
     * and take every frame's reply from the others.
     * The beacon counter prevents an old uplink being paired with a new frame. */
    unsigned entry = (unsigned)(e - m->reg.e);
    if (have_rx && m->cycle_ready && n == m->cycle_wire_len && u.beacon_ctr == m->beacon_ctr &&
        (u.diag.health & HEALTH_CLOCK_XTAL) &&
        (!m->reply_valid || u.diag.sync_age_ms > m->reply_age_ms ||
         (u.diag.sync_age_ms == m->reply_age_ms &&
          entry == m->cycle_seq % CHECKPOINT_PERIOD_BEACONS && m->reply_entry != entry))) {
        for (unsigned j = 0; j < SLOT_COUNT; j++) {
            if (m->schedule[j] != (SLOT_ROLE_DATA | entry)) { continue; }
            uint64_t start = m->cycle_tx + (SLOT_OFFSET_MS + j * SLOT_LEN_MS) * TPM;
            /* Keep the existing guard for the two radio event delays. */
            if (rx_tick < start || rx_tick > start + (SLOT_LEN_MS - LBT_LISTEN_MS) * TPM) { continue; }
            m->reply_rx = rx_tick; m->reply_entry = (uint8_t)entry;
            m->reply_age_ms = u.diag.sync_age_ms;
            m->reply_link = e->link_id; m->reply_valid = 1;
            break;
        }
    }
    uint32_t now_ms = board_millis();
    e->heard_ms = now_ms;
    e->rssi = rssi;
    e->snr = snr;
    e->diag = u.diag;
    e->diag_valid = 1;
    if (!(u.flags & EVENT_TIME_UNKNOWN) && u.kind != 0u) {
        uint64_t now_t = capture_now64();
        e->lat_ms = now_t > u.tick ? (uint32_t)((now_t - u.tick) / TPM) : 0u;
    }

    if (u.kind != UL_KIND_SYNC) {
        if (!e->have_ack) { e->ack_seq = (uint16_t)(u.ev_seq - 1u); e->have_ack = 1; }
        int16_t d = (int16_t)(uint16_t)(u.ev_seq - e->ack_seq);
        if (d > 0 && u.ev_seq < e->ack_seq) { d--; } /* the sensor skips 0 when ev_seq wraps */
        if (d > 0) {
            /* The sensor only drops a packet the beacon acked, so a jump means a
             * wrongly applied ACK: take it (the console sees any seq gap) rather
             * than stall the sensor forever. */
            if (d > 1) { el_note(EL_ACK_GAP); }
            /* Queue full: leave ack_seq; the sensor re-sends next cycle. */
            if (mq_push_uplink(m->q, e->id, e->sensor_boot, &u, rssi, snr)) { e->ack_seq = u.ev_seq; }
        }
    }
    e->last_state = reg_state(e, now_ms);
    emit_sensor(e, now_ms);
    pu_emit_join(e->id, (e->diag.health & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED ? PU_JOIN_READY : PU_JOIN_SYNC);
}

static void receive_one(mac_master_t *m)
{
    uint8_t buf[WIRE_MAX];
    float rssi = 0, snr = 0;
    int n = radio_receive(buf, sizeof(buf), &rssi, &snr);
    if (n > 0) {
        uint64_t cap = 0;
        int have = capture_dio1_get(&cap);
        on_uplink(m, buf, n, rssi, snr, have, cap);
    }
}

static int beacon_channel_ready(mac_master_t *m, uint64_t deadline)
{
    /* Leave enough time to perform both enabled checks; never bypass CAD. */
    uint64_t checks = (CAD_TIMEOUT_MS + (radio_lbt_enabled() ? LBT_LISTEN_MS : 0u)) * TPM;
    mac_radio_limit(deadline);
    while (capture_now64() + checks < deadline) {
        uint32_t remain = (uint32_t)((deadline - capture_now64() - checks) / TPM);
        int rx = radio_rx_settle(RX_HEADER_WAIT_MS, remain);
        if (rx == RADIO_RX_BUSY) { return 0; }
        if (rx == RADIO_RX_PACKET) { receive_one(m); }
        if (radio_needs_reset() || capture_now64() + checks >= deadline) { return 0; }
        if (radio_lbt_enabled() && radio_lbt_listen() != RADIO_LBT_QUIET) { continue; }
        int cad = radio_cad();
        if (cad == RADIO_CAD_ERROR) { return 0; }
        if (cad == RADIO_CAD_CLEAR) { return capture_now64() < deadline; }
    }
    return 0;
}

static void send_beacon(mac_master_t *m)
{
    uint64_t deadline = m->next_beacon + BEACON_ACCESS_MS * TPM;
    admission_beacon_t b = {0};
    b.base.seq = m->seq++;
    b.base.flags = m->m_tx_valid ? BEACON_TX_PREV_VALID : 0u;
    b.base.m_tx_prev = m->m_tx_last;
    b.base.cp_req = m->cp_left ? m->cp_id : 0u;
    master_admission_schedule(&m->admission, &m->reg, board_millis(), &b, m->schedule);
    if (m->m_tx_valid && m->reply_valid && m->reply_entry < MAX_NODES &&
        m->reg.e[m->reply_entry].link_id == m->reply_link &&
        b.base.slot[m->reply_entry].link_id == m->reply_link) {
        b.base.flags |= BEACON_RX_PREV_VALID;
        b.base.m_rx_prev = m->reply_rx; b.base.rx_entry = m->reply_entry;
    }
    m->m_tx_valid = m->cycle_ready = m->reply_valid = 0;
    if (!beacon_channel_ready(m, deadline)) { radio_unlimit(); return; }
    uint8_t tx[WIRE_ADMISSION];
    int plen = (b.base.flags & BEACON_ADMISSION) ? (int)sizeof(b) : (int)sizeof(b.base);
    int n = sec_seal(tx, sizeof(tx), PKT_TYPE_BEACON, NODE_MASTER, &b, plen);
    uint64_t cap;
    (void)capture_dio1_get(&cap);
    if (n > 0 && capture_now64() <= deadline) {
        uint64_t finish = deadline + (TX_SETUP_MS + SLOT_GUARD_MS) * TPM +
                          (uint64_t)LORA_TOA_US((unsigned)n) * (TPM / 1000u);
        if (radio_tx_start(tx, n, mac_deadline_us(finish)) == 0) {
            m->tx_state = MASTER_TX_BEACON;
            m->beacon_ctr = sec_wire_counter(tx);
            m->cycle_seq = b.base.seq; m->cycle_wire_len = (uint8_t)n;
            if (m->cp_left) { m->cp_left--; }
            return;
        }
    }
    radio_unlimit();
    if (!radio_needs_reset()) { radio_start_rx(); }
}

static void send_offer(mac_master_t *m)
{
    if (!m->cycle_ready) { return; }
    for (unsigned j = 0; j < SLOT_COUNT; j++) {
        if (m->slot_done & (1u << j) || SLOT_ROLE(m->schedule[j]) != SLOT_ROLE_OFFER) { continue; }
        unsigned i = SLOT_ENTRY(m->schedule[j]);
        uint64_t start = m->cycle_tx + (SLOT_OFFSET_MS + j * SLOT_LEN_MS) * TPM;
        uint64_t late = start + SLOT_LATE_MS * TPM;
        uint64_t listen = radio_lbt_enabled() ? LBT_LISTEN_MS * TPM : 0;
        if (capture_now64() + listen < start) { return; }
        m->slot_done |= (uint8_t)(1u << j);
        offer_pl_t offer;
        if (!master_admission_offer(&m->admission, i, m->beacon_ctr, &offer) || capture_now64() > late) { continue; }
        mac_radio_limit(late);
        if ((listen && radio_lbt_listen() != RADIO_LBT_QUIET) || radio_rx_settle(0, 0) != RADIO_RX_IDLE ||
            radio_cad() != RADIO_CAD_CLEAR) { radio_unlimit(); continue; }
        uint8_t tx[WIRE_OFFER];
        int n = sec_seal(tx, sizeof(tx), PKT_TYPE_OFFER, 0, &offer, sizeof(offer));
        uint64_t unused; (void)capture_dio1_get(&unused);
        uint64_t finish = start + (SLOT_LEN_MS - SLOT_GUARD_MS - LBT_LISTEN_MS) * TPM;
        if (n > 0 && capture_now64() <= late && radio_tx_start(tx, n, mac_deadline_us(finish)) == 0) {
            master_admission_offer_started(&m->admission, i,
                j + 1u < SLOT_COUNT && SLOT_ROLE(m->schedule[j+1]) == SLOT_ROLE_CONFIRM);
            m->tx_state = MASTER_TX_OFFER; return;
        }
        radio_unlimit();
        if (!radio_needs_reset()) { radio_start_rx(); }
    }
}

static void radio_recovery(mac_master_t *m)
{
    if (!radio_needs_reset()) { return; }
    uint32_t now = board_millis();
    if (!retry_due(&m->retry, now)) { return; }
    el_radio_recovery_begin(el_radio_recovery_reason());
    int ok = radio_begin() == 0 && radio_start_rx() == 0;
    el_radio_recovery_end(ok);
    retry_later(&m->retry, board_millis(), RADIO_RESET_BACKOFF_MAX_MS);
}

int mac_master_step(mac_master_t *m)
{
    if (m->tx_state != MASTER_TX_IDLE) {
        int done = radio_tx_poll();
        if (!done) { return 0; }
        master_tx_state_t completed = m->tx_state;
        m->tx_state = MASTER_TX_IDLE;
        uint64_t cap;
        int have = capture_dio1_get(&cap);
        if (completed == MASTER_TX_BEACON && done > 0 && have) {
            m->m_tx_last = m->cycle_tx = cap;
            m->m_tx_valid = m->cycle_ready = 1;
            m->slot_done = 0;
            m->retry.delay_ms = RADIO_BOOT_RETRY_MS;
            board_led_toggle();
        }
        if (completed == MASTER_TX_BEACON && done < 0) { el_note(EL_BEACON_FAIL); }
    }
    if (!radio_needs_reset()) {
        radio_limit(board_micros() + 7000u);
        receive_one(m);
        radio_unlimit();
    }
    int sent = 0;
    if (capture_now64() >= m->next_beacon) {
        if (!radio_needs_reset()) { send_beacon(m); }
        else { m->m_tx_valid = m->cycle_ready = m->reply_valid = 0; m->seq++; }
        m->next_beacon += BEACON_PERIOD_TICKS;
        if (m->next_beacon <= capture_now64()) { m->next_beacon = capture_now64() + BEACON_PERIOD_TICKS; }
        sent = 1;
    }
    if (m->tx_state == MASTER_TX_IDLE && !radio_needs_reset()) { send_offer(m); }
    if (m->tx_state == MASTER_TX_IDLE && m->cycle_ready && capture_now64() >= m->cycle_tx + (SLOT_OFFSET_MS + SLOT_COUNT * SLOT_LEN_MS) * TPM) {
        meas_service(board_millis()); m->cycle_ready = 0;
    }
    if (m->tx_state == MASTER_TX_IDLE && !m->cycle_ready) { radio_recovery(m); }
    return sent;
}
