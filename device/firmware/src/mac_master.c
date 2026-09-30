#include "mac_master.h"

#include <string.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "proto_usb.h"
#include "radio.h"
#include "secure.h"

#define TPM ((uint64_t)TICKS_PER_MS)
#define BEACON_PERIOD_TICKS ((uint64_t)BEACON_PERIOD_MS * TPM)

void mac_master_init(mac_master_t *m, mq_t *q)
{
    memset(m, 0, sizeof(*m));
    reg_init(&m->reg);
    m->q = q;
    m->next_beacon = capture_now64();
    m->reset_after_ms = board_millis();
    m->reset_backoff_ms = 1000u;
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

    uint32_t now_ms = board_millis();
    reg_entry_t *e = reg_lookup(&m->reg, meta.node_id);
    if (!e) {
        int why = 0;
        e = reg_claim(&m->reg, meta.node_id, now_ms, &why);
        if (!e) {
            el_note(why == REG_COLLISION ? EL_ID_COLLISION : EL_REGISTRY_FULL);
            return;
        }
    }
    if (!sec_replay(&e->rx, meta.boot_id, meta.ctr)) { e->sec_drop++; return; }
    /* Session binding: records name the master session their ticks belong to. */
    if (u.master_boot_id != sec_boot_id()) { e->sec_drop++; return; }

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

/* Listen before talk, bounded: finish a reception in progress, re-sense a busy
 * channel for up to BEACON_LBT_MAX_MS, then send regardless — every sensor syncs
 * on the beacon, and sync uses the real TxDone, not the nominal time. */
static void beacon_lbt(mac_master_t *m)
{
    uint32_t t0 = board_millis();
    for (;;) {
        if (radio_rx_settle(RX_HEADER_WAIT_MS, RX_DRAIN_MAX_MS) == RADIO_RX_PACKET) { receive_one(m); }
        if ((uint32_t)(board_millis() - t0) >= BEACON_LBT_MAX_MS) { return; }
        if (!radio_cad()) { return; }
    }
}

static void send_beacon(mac_master_t *m)
{
    beacon_lbt(m);
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
    if ((int32_t)(now - m->reset_after_ms) < 0) { return; }
    el_note(EL_RADIO_RESET);
    radio_begin();
    radio_start_rx();
    m->reset_after_ms = now + m->reset_backoff_ms;
    m->reset_backoff_ms = m->reset_backoff_ms * 2u > RADIO_RESET_BACKOFF_MAX_MS
                          ? RADIO_RESET_BACKOFF_MAX_MS : m->reset_backoff_ms * 2u;
}

int mac_master_step(mac_master_t *m)
{
    int sent = 0;
    uint64_t now = capture_now64();
    if (now >= m->next_beacon) {
        send_beacon(m);
        m->next_beacon += BEACON_PERIOD_TICKS;
        if (m->next_beacon <= capture_now64()) { m->next_beacon = capture_now64() + BEACON_PERIOD_TICKS; }
        sent = 1;
        if (m->m_tx_valid) { m->reset_backoff_ms = 1000u; } /* the radio works again */
    }
    receive_one(m);
    radio_recovery(m);
    return sent;
}
