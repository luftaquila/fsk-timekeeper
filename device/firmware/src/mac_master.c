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
    radio_start_rx();
}

void mac_master_checkpoint(mac_master_t *m)
{
    m->cp_id = (uint8_t)(m->cp_id % 255u + 1u); /* 1..255, never the "no request" 0 */
    m->cp_left = CP_REQ_BEACONS;
}

void mac_master_session(mac_master_t *m)
{
    reg_session(&m->reg);
    m->m_tx_valid = 0;
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
    d.sync_age_ms = e->diag.sync_age_ms;
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
    }
}

static void on_uplink(mac_master_t *m, const uint8_t *buf, int n, float rssi, float snr)
{
    if (SEC_VT_VER(buf[0]) != PROTO_VER) {
        /* Another version counts only when the fleet key opens it: a foreign
         * LoRa device on our channel and sync word is not an outdated board. */
        if (sec_authentic_any_version(buf, n)) { m->ver_drop++; }
        return;
    }
    if (SEC_VT_TYPE(buf[0]) != PKT_TYPE_UPLINK || n < WIRE_UPLINK) { return; }
    sec_meta_t meta;
    uplink_pl_t u;
    if (sec_unseal(buf, n, &meta, &u, sizeof(u)) != 0) { m->auth_drop++; return; }

    /* Reject other master sessions before allocating entries or changing replay state. */
    if (u.master_boot_id != sec_boot_id()) { m->auth_drop++; return; }

    uint32_t now_ms = board_millis();
    reg_entry_t *e = reg_lookup(&m->reg, meta.node_id);
    if (!e) {
        int why = 0;
        e = reg_claim(&m->reg, meta.node_id, now_ms, &why);
        if (!e) {
            el_note(EL_REGISTRY_FULL);
            return;
        }
    }
    if (!sec_replay(&e->rx, meta.boot_id, meta.ctr)) { e->sec_drop++; return; }

    if (!e->have_boot || e->sensor_boot != meta.boot_id) {
        /* First uplink of this sensor boot (or of this master session): its
         * oldest unacked packet is what arrives first. */
        e->have_boot = 1;
        e->sensor_boot = meta.boot_id;
        e->ack_seq = (uint16_t)(u.ev_seq - 1u);
    }
    e->heard_ms = now_ms;
    e->rssi = rssi;
    e->snr = snr;
    e->diag = u.diag;
    if (!(u.flags & EVENT_TIME_UNKNOWN) && u.kind != 0u) {
        uint64_t now_t = capture_now64();
        e->lat_ms = now_t > u.tick ? (uint32_t)((now_t - u.tick) / TPM) : 0u;
    }

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
    e->last_state = reg_state(e, now_ms);
    emit_sensor(e, now_ms);
}

static void receive_one(mac_master_t *m)
{
    uint8_t buf[WIRE_MAX];
    float rssi = 0, snr = 0;
    int n = radio_receive(buf, sizeof(buf), &rssi, &snr);
    if (n > 0) { on_uplink(m, buf, n, rssi, snr); }
}

/* Finish a reception, optionally listen to RSSI, then check CAD. Retry a busy
 * channel until BEACON_CHANNEL_RETRY_MS has elapsed. This cutoff is checked
 * after drain/listen, so total duration can exceed it. At the cutoff omit CAD
 * and allow TX unless RSSI is loud; a CAD error always rejects this beacon.
 * Sensor sync uses the actual TxDone, including any channel-access delay. */
static int beacon_channel_ready(mac_master_t *m)
{
    uint32_t t0 = board_millis();
    for (;;) {
        if (radio_rx_settle(RX_HEADER_WAIT_MS, RX_DRAIN_MAX_MS) == RADIO_RX_PACKET) { receive_one(m); }
        int loud = radio_lbt_enabled() && radio_lbt_listen() != RADIO_LBT_QUIET;
        if ((uint32_t)(board_millis() - t0) >= BEACON_CHANNEL_RETRY_MS) { return !loud; }
        if (loud) { continue; }
        int cad = radio_cad();
        if (cad == RADIO_CAD_ERROR) { return 0; }
        if (cad == RADIO_CAD_CLEAR) { return 1; }
    }
}

static void send_beacon(mac_master_t *m)
{
    if (!beacon_channel_ready(m)) {
        m->m_tx_valid = 0;
        m->seq++;
        radio_start_rx();
        return;
    }
    beacon_pl_t b;
    b.seq = m->seq;
    b.flags = m->m_tx_valid ? BEACON_TX_PREV_VALID : 0u;
    b.m_tx_prev = m->m_tx_last;
    b.cp_req = m->cp_left ? m->cp_id : 0u;
    if (m->cp_left) { m->cp_left--; }
    reg_slot_table(&m->reg, board_millis(), b.slot);

    uint8_t tx[WIRE_BEACON];
    int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_BEACON, NODE_MASTER, &b, sizeof(b));
    m->m_tx_valid = 0;
    if (wlen > 0) {
        uint64_t cap;
        (void)capture_dio1_get(&cap); /* drop an older DIO1 edge (CAD done, RxDone) */
        if (radio_transmit(tx, wlen) == 0 && capture_dio1_get(&cap)) {
            m->m_tx_last = cap;
            m->m_tx_valid = 1;
        } else {
            el_note(EL_BEACON_FAIL);
        }
        board_led_toggle();
    }
    radio_start_rx();
    m->seq++;
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
    int sent = 0;
    uint64_t now = capture_now64();
    if (now >= m->next_beacon) {
        if (!radio_needs_reset()) { send_beacon(m); }
        else { m->m_tx_valid = 0; m->seq++; }
        meas_service(board_millis());
        m->next_beacon += BEACON_PERIOD_TICKS;
        if (m->next_beacon <= capture_now64()) { m->next_beacon = capture_now64() + BEACON_PERIOD_TICKS; }
        sent = 1;
        if (m->m_tx_valid) { m->retry.delay_ms = RADIO_BOOT_RETRY_MS; } /* the radio works again */
    }
    if (!radio_needs_reset()) { receive_one(m); }
    radio_recovery(m);
    return sent;
}
