/* One master + up to 6 sensors, single channel, single timebase — single
 * binary, role decided at runtime from USB (DESIGN §8).
 *
 * MASTER = the node a PC enumerates over USB-CDC. Speaks the FSK-WL line
 * protocol (proto_usb.c): beacons once per second to keep the sensors synced,
 * auto-registers sensors by chip id on their first authenticated packet, reports
 * sensor EVENTs ("E ...") and per-sensor diagnostics ("D ...") keyed by chip id,
 * and a 1 Hz heartbeat ("H ...").
 *
 * SENSOR = remote node (no PC host): transmits under its own chip id (DESIGN
 * §2.3), syncs to beacons (single-direction time sync), captures a SENSOR edge
 * in hardware, maps it to master time and transmits an EVENT (reliable: CAD +
 * ACK + retransmit). Reports its sync health in a STATUS once per
 * STATUS_PERIOD_S beacons at a hashed in-gap offset.
 * -DNODE_DEBUG prints the chip id + resolved role.
 */
#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "radio.h"
#include "usb.h"
#include "capture.h"
#include "protocol.h"
#include "proto_usb.h"
#include "config.h"
#include "node_id.h"
#include "meas.h"
#include "secure.h"
#include "keystore.h"
#include "gps.h"

#define TICKS_PER_MS 16000u /* TIMER1 is 16 MHz */
#define OFF_HIST     8u     /* offset ring depth for the skew estimate */

#if defined(NODE_DEBUG)
static void debug_id(const char *role)
{
    static uint32_t dl;
    if ((uint32_t)(board_millis() - dl) < 1000u) { return; }
    dl = board_millis();
    char l[40];
    /* hand-built (no %llX needed); 32-bit halves of the chip id. */
    static const char hex[] = "0123456789ABCDEF";
    char *p = l;
    *p++ = '#'; *p++ = ' ';
    uint32_t hi = node_devid_hi(), lo = node_devid_lo();
    for (int i = 7; i >= 0; i--) { *p++ = hex[(hi >> (i * 4)) & 0xF]; }
    for (int i = 7; i >= 0; i--) { *p++ = hex[(lo >> (i * 4)) & 0xF]; }
    *p++ = ' ';
    while (*role) { *p++ = *role++; }
    *p++ = '\n'; *p = '\0';
    usb_write(l);
}
#endif

/* ===== role by USB ====================================================== */

/* Pump the USB stack up to ROLE_SETTLE_MS for a PC to enumerate us. Returns 1
 * (master) as soon as a host is present, else 0 (sensor) when the window closes.
 * Role is decided ONCE here at boot. There is deliberately no live re-check /
 * auto-reset: USB host presence (tud_mounted) can drop when the host isn't
 * actively holding the CDC port (e.g. USB autosuspend), and an auto-reset on that
 * rebooted the master — resetting the beacon sequence and wrecking sensor sync.
 * To change a board's role, reset/power-cycle it (you reboot it on reconnect
 * anyway). */
static int role_decide_master(void)
{
    uint32_t t0 = board_millis();
    for (;;) {
        usb_task();
        if (usb_host_present()) { return 1; }
        if ((uint32_t)(board_millis() - t0) >= ROLE_SETTLE_MS) { return 0; }
    }
}

/* Write the fleet key from a PU_CMD_SETKEY command to the flash keystore and
 * reload it live. Common to both roles — any board is provisioned by plugging it
 * into USB and sending `K <64-hex>`. Write-only: there is no read-back command,
 * so the key can't be exfiltrated over serial. */
static void provision_key(void)
{
    if (keystore_write(pu_setkey()) == 0) {
        sec_reload();
        pu_emit_ack("K");
    } else {
        pu_emit_err("keyfail");
    }
}

/* ===== sensor =========================================================== */

/* Perform one EVENT delivery attempt. The caller-owned pending queue preserves
 * the payload and schedules retries until ACK or bounded expiry. */
static int sensor_try_send_event(const event_pl_t *e, uint32_t id,
                                 uint32_t master_boot_id, sec_replay_t *from_master)
{
    if (!radio_lbt_clear()) { return 0; }
    uint8_t tx[WIRE_EVENT];
    int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_EVENT, id, e, sizeof(*e));
    if (wlen < 0) { return 0; }
    radio_transmit(tx, wlen);
    radio_start_rx();

    uint32_t t0 = board_millis();
    while ((uint32_t)(board_millis() - t0) < 120u) {
        uint8_t b[WIRE_MAX];
        int n = radio_receive(b, sizeof(b));
        if (n >= WIRE_ACK && SEC_VT_TYPE(b[0]) == PKT_TYPE_ACK) {
            sec_meta_t m;
            ack_pl_t a;
            if (sec_unseal(b, n, &m, &a, sizeof(a)) == 0 &&
                m.node_id == NODE_MASTER &&
                m.boot_id == master_boot_id &&
                a.node_id == id && a.ev_seq == e->ev_seq &&
                a.sensor_boot_id == sec_boot_id() &&
                a.ev_master_t == e->ev_master_t &&
                sec_replay(from_master, m.boot_id, m.ctr)) {
                return 1;
            }
        }
    }
    radio_start_rx();
    return 0;
}

/* Fire-and-forget diagnostics uplink. The LBT sense just before TX turns a near-
 * overlap with another sensor's STATUS into a short deferral rather than a
 * collision (DESIGN §2.8). */
static void sensor_send_status(uint32_t id, uint8_t *sseq, uint64_t off, int32_t skew,
                               uint16_t rx_miss, uint16_t gap, uint16_t sync_age_ms,
                               uint16_t capture_overflow, uint16_t event_drop,
                               uint8_t health_flags)
{
    status_pl_t s;
    s.seq = (*sseq)++;
    s.offset_tick = (int64_t)off;
    s.skew_ppm = (int16_t)(skew > 32767 ? 32767 : (skew < -32768 ? -32768 : skew));
    s.rx_miss = rx_miss;
    s.beacon_gap = (uint8_t)(gap > 255u ? 255u : gap);
    /* Cell estimate: VDDH (via SAADC VDDHDIV5) + the W5 diode drop. Sampled here,
     * right before TX, so VDDH reflects near-peak load. */
    s.batt_mv = (uint16_t)(meas_vddh_mv() + BATT_DIODE_DROP_MV);
    s.temp_c10 = meas_temp_c10();
    s.sync_age_ms = sync_age_ms;
    s.capture_overflow = capture_overflow;
    s.event_drop = event_drop;
    s.flags = health_flags;
    uint8_t tx[WIRE_STATUS];
    int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_STATUS, id, &s, sizeof(s));
    if (wlen < 0) { radio_start_rx(); return; } /* tx counter exhausted */
    /* LBT (KR920): transmit only on a clear channel. Busy → back off a short
     * id-derived jitter and let this cycle pass; STATUS is loss-tolerant and the
     * next cycle re-hashes to a fresh phase. */
    if (!radio_lbt_clear()) { board_delay_ms(3u + (uint32_t)(id & 7u)); radio_start_rx(); return; }
    radio_transmit(tx, wlen);
    radio_start_rx();
}

/* Cheap 32-bit mix for the STATUS phase: spreads (id, cycle) across the synced
 * cycle so sensors rarely overlap, and re-randomises every cycle so any overlap
 * is transient rather than a permanent pairing (DESIGN §2.8). */
static uint32_t hash32(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u + b * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

/* Sensor-local capture tick → master time. Offset-only unless the skew estimate is
 * validated (DESIGN §2.5): then the clock drift since the offset's anchor (sync_ref_tick)
 * is corrected. Guards: correct only forward of the anchor (keeps the subtract non-negative
 * — no implementation-defined large-uint64→int64 cast — and an event before the anchor, e.g.
 * a latched pre-sync edge, falls back to offset-only here instead of extrapolating backward;
 * the actual drop of fully-unsynced events is the caller's capture_sensor_get() && have_off
 * ordering, not this function), and only within a bounded window so a long gap doesn't
 * extrapolate wildly. skew_valid already implies |skew_ppm| ≤ SKEW_CLAMP_PPM. */
static uint64_t ev_to_master_t(uint64_t ev_tick, uint64_t cur_off, uint64_t sync_ref_tick,
                               int32_t skew_ppm, int skew_valid)
{
    uint64_t mt = cur_off + ev_tick;
    if (skew_valid && ev_tick >= sync_ref_tick &&
        (ev_tick - sync_ref_tick) <= (uint64_t)SKEW_MAX_EXTRAP_MS * TICKS_PER_MS) {
        int64_t corr = (int64_t)(ev_tick - sync_ref_tick) * skew_ppm / 1000000;
        if (corr >= 0) { mt += (uint64_t)corr; }
        else           { mt -= (uint64_t)(-corr); }
    }
    return mt;
}

typedef struct {
    event_pl_t event;
    uint32_t first_ms;
    uint32_t retry_ms;
} sensor_pending_t;

static uint16_t sensor_sync_age_ms(int have_off, uint64_t sync_ref_tick, uint64_t now_tick)
{
    if (!have_off || now_tick < sync_ref_tick) { return UINT16_MAX; }
    uint64_t age = (now_tick - sync_ref_tick) / TICKS_PER_MS;
    return age > UINT16_MAX ? UINT16_MAX : (uint16_t)age;
}

static uint8_t sensor_health(int have_off, int skew_valid, uint64_t sync_ref_tick,
                             uint64_t now_tick)
{
    uint8_t flags = 0;
    uint16_t age = sensor_sync_age_ms(have_off, sync_ref_tick, now_tick);
    if (age <= SYNC_TTL_MS) { flags |= HEALTH_SYNC_VALID; }
    if (skew_valid) { flags |= HEALTH_SKEW_VALID; }
    if (board_hfclk_xtal()) { flags |= HEALTH_CLOCK_XTAL; }
    flags |= HEALTH_CAPTURE_OK;
    return flags;
}

static void sat_inc_u16(uint16_t *value)
{
    if (*value != UINT16_MAX) { (*value)++; }
}

/* Loss reports use the same bounded reliable queue as captures. Only the
 * unsent accumulator may change; a queued packet is immutable until its ACK. */
static void sensor_note_loss(event_pl_t *loss, int *pending, const event_pl_t *event)
{
    if (!*pending) { *loss = *event; *pending = 1; }
    else {
        if ((int32_t)(event->capture_seq - loss->capture_seq) < 0) {
            loss->capture_seq = event->capture_seq; loss->ev_master_t = event->ev_master_t;
        }
        if ((int32_t)(event->end_seq - loss->end_seq) > 0) {
            loss->end_seq = event->end_seq; loss->end_tick = event->end_tick;
        }
    }
    loss->flags |= EVENT_LOSS | (event->flags & EVENT_TIME_UNKNOWN);
}

static void run_sensor(int st)
{
    uint64_t prev_l_rx = 0, cur_off = 0;
    uint8_t prev_seq = 0;
    const uint32_t my_id = node_sender_id(); /* our 32-bit on-air identity (chip id low word) */
    int have_prev = 0, have_off = 0;
    int timing_ready = 0;
    uint64_t timing_ready_tick = 0;
    uint16_t ev_seq = 0;
    sensor_pending_t pending[SENSOR_EVENT_QUEUE_LEN];
    unsigned pending_head = 0, pending_tail = 0, pending_n = 0;
    uint16_t event_drop = 0;
    event_pl_t loss = {0};
    int loss_pending = 0;
    uint32_t checkpoint_ms = board_millis() - STATUS_PERIOD_S * 1000u;
    uint8_t cp_pending = 0, cp_answered = 0; /* beacon checkpoint request: id awaiting our answer / last id answered */

    int64_t off_hist[OFF_HIST];
    uint64_t lrx_hist[OFF_HIST];
    unsigned hist_n = 0, hist_i = 0;
    int32_t cur_skew = 0;          /* measured skew (ppm), reported raw (i16-clamped) in STATUS */
    int skew_valid = 0;            /* cur_skew is plausible + enough samples → ok for event-time correction */
    uint64_t sync_ref_tick = 0;    /* sensor tick cur_off is anchored at (= prev_l_rx when cur_off was computed) */
    uint16_t rx_miss = 0, beacon_gap = 0;
    uint8_t status_seq = 0;
    int sent_status = 0; /* already sent STATUS in the current beacon frame (reset on each beacon) */

    uint32_t last_blink = board_millis();
    uint8_t buf[WIRE_MAX];
    sec_replay_t from_master = {0}; /* replay window for the master's beacons/acks */
    uint32_t master_boot_id = 0;    /* current master session, learned from beacons */
    int have_master_session = 0;    /* a master session has been latched (boot_id is RNG, so 0 is a valid value — don't use master_boot_id==0 as a sentinel) */

    if (st == 0) { radio_start_rx(); }

    for (;;) {
        usb_task();
        /* Serial provisioning works regardless of radio/role: plug the board in
         * and send `K <64-hex>` (also ?ID to identify it). */
        int uc;
        while ((uc = usb_read_byte()) >= 0) {
            switch (pu_feed(uc)) {
            case PU_CMD_SETKEY: provision_key(); break;
            case PU_CMD_ID: pu_emit_identity(node_devid_hi(), node_devid_lo()); pu_emit_ack("ID"); break;
            case PU_CMD_PING: pu_emit_ack("PING"); break;
            case PU_CMD_BAD: pu_emit_err("badcmd"); break;
            default: break;
            }
        }
#if defined(NODE_DEBUG)
        debug_id("sensor");
#endif
        if (st != 0) {
            uint32_t now = board_millis();
            if ((uint32_t)(now - last_blink) >= 1000u) { last_blink = now; board_led_toggle(); }
            continue;
        }
        capture_now64();

        uint64_t previous_off = cur_off, previous_ref = sync_ref_tick;
        int previous_have_off = have_off, previous_skew_valid = skew_valid;
        int32_t previous_skew = cur_skew;
        float rssi, snr;
        int n = radio_receive_q(buf, sizeof(buf), &rssi, &snr);
        if (n >= WIRE_BEACON && SEC_VT_TYPE(buf[0]) == PKT_TYPE_BEACON) {
            uint64_t l_rx = 0;
            capture_dio1_get(&l_rx);
            sec_meta_t m;
            beacon_pl_t b;
            if (sec_unseal(buf, n, &m, &b, sizeof(b)) == 0 &&
                m.node_id == NODE_MASTER &&
                sec_replay(&from_master, m.boot_id, m.ctr)) {
                board_led_toggle();
                if (!have_master_session || m.boot_id != master_boot_id) {
                    /* New master session (reboot) or first contact. The old offset/skew
                     * relate to a now-defunct master timebase (its TIMER restarts on
                     * boot), so drop sync and re-baseline on THIS beacon — distinct from
                     * an in-session gap, which preserves the estimator. Without this an
                     * event arriving before the next consecutive beacon would be stamped
                     * with a stale cross-session offset yet bound to the new session.
                     * rx_miss is cumulative-since-boot so it is kept; beacon_gap (current
                     * streak) is cleared. */
                    pending_head = pending_tail = pending_n = 0;
                    loss_pending = 0;
                    timing_ready = 0;
                    have_master_session = 1;
                    master_boot_id = m.boot_id;
                    have_off = 0;
                    hist_n = 0; hist_i = 0;
                    cur_skew = 0; skew_valid = 0; sync_ref_tick = 0;
                    beacon_gap = 0;
                    cp_pending = 0; cp_answered = 0;
                } else if (have_prev) {
                    if (b.seq == (uint8_t)(prev_seq + 1u)) {
                        cur_off = b.m_tx_prev + T_AIR_REF_TICKS - prev_l_rx;
                        sync_ref_tick = prev_l_rx; /* anchor for skew extrapolation (l_rx[N-1]) */
                        have_off = 1;
                        beacon_gap = 0;
                        off_hist[hist_i] = (int64_t)cur_off;
                        lrx_hist[hist_i] = prev_l_rx;
                        hist_i = (hist_i + 1u) % OFF_HIST;
                        if (hist_n < OFF_HIST) { hist_n++; }
                        if (hist_n >= 2u) {
                            unsigned newest = (hist_i + OFF_HIST - 1u) % OFF_HIST;
                            unsigned oldest = (hist_n < OFF_HIST) ? 0u : hist_i;
                            int64_t doff = off_hist[newest] - off_hist[oldest];
                            int64_t dl = (int64_t)(lrx_hist[newest] - lrx_hist[oldest]);
                            if (dl >= (int64_t)SKEW_MIN_DL_TICKS) {
                                int64_t cand = (doff * 1000000) / dl;
                                /* diag: keep the real measurement for STATUS, clamped only to the
                                 * i16 wire range so a fault (RC fallback ~10000 ppm) stays visible. */
                                cur_skew = (int32_t)(cand > 32767 ? 32767 : (cand < -32768 ? -32768 : cand));
                                /* apply gate: trust it for event timestamps only if it's a plausible
                                 * XO drift and we have enough samples. */
                                skew_valid = (hist_n >= SKEW_MIN_SAMPLES) &&
                                             (cand <= SKEW_CLAMP_PPM) && (cand >= -SKEW_CLAMP_PPM);
                            }
                        }
                    } else {
                        uint8_t miss = (uint8_t)(b.seq - prev_seq - 1u);
                        if ((uint16_t)(rx_miss + miss) >= rx_miss) { rx_miss += miss; }
                        beacon_gap = miss;
                    }
                }
                /* Checkpoint on demand: answer each request id once (config.h CP_REQ_BEACONS). */
                if (b.cp_req != 0u && b.cp_req != cp_answered) { cp_pending = b.cp_req; }
                prev_l_rx = l_rx;
                prev_seq = b.seq;
                have_prev = 1;
                sent_status = 0; /* new beacon frame — re-evaluate the STATUS slot */
            }
        }

        if (!board_hfclk_xtal()) {
            // A recovered oscillator needs fresh anchors; its old skew estimate
            // cannot certify captures spanning the clock-source transition.
            have_prev = have_off = skew_valid = 0;
            previous_have_off = previous_skew_valid = 0;
            hist_n = hist_i = 0;
        }
        uint64_t health_tick = capture_now64();
        if (!timing_ready && sensor_health(have_off, skew_valid, sync_ref_tick, health_tick) == HEALTH_EVENT_REQUIRED) {
            timing_ready = 1;
            timing_ready_tick = health_tick;
        }
        uint64_t first_tick, last_tick;
        uint32_t first_seq, last_seq;
        if (capture_sensor_loss(&first_tick, &last_tick, &first_seq, &last_seq) && timing_ready) {
            event_pl_t dropped = { .master_boot_id = master_boot_id, .capture_seq = first_seq, .end_seq = last_seq,
                .ev_master_t = ev_to_master_t(first_tick, cur_off, sync_ref_tick, cur_skew, skew_valid),
                .end_tick = ev_to_master_t(last_tick, cur_off, sync_ref_tick, cur_skew, skew_valid),
                .flags = HEALTH_EVENT_REQUIRED };
            if (sensor_health(have_off, skew_valid, sync_ref_tick, first_tick) != HEALTH_EVENT_REQUIRED)
                dropped.flags |= EVENT_TIME_UNKNOWN;
            sensor_note_loss(&loss, &loss_pending, &dropped);
        }
        if (loss_pending && pending_n < SENSOR_EVENT_QUEUE_LEN) {
            sensor_pending_t *p = &pending[pending_head];
            p->event = loss; p->event.ev_seq = ev_seq++;
            p->first_ms = p->retry_ms = board_millis();
            pending_head = (pending_head + 1u) % SENSOR_EVENT_QUEUE_LEN; pending_n++;
            loss_pending = 0;
        }
        uint64_t ev_tick;
        uint32_t capture_seq;
        int capture_xtal;
        while (capture_sensor_get(&ev_tick, &capture_seq, &capture_xtal)) {
            if (!timing_ready || ev_tick < timing_ready_tick) continue;
            int older = ev_tick < sync_ref_tick;
            uint64_t ref = older ? previous_ref : sync_ref_tick;
            uint64_t off = older ? previous_off : cur_off;
            int valid_skew = older ? previous_skew_valid : skew_valid;
            int32_t skew = older ? previous_skew : cur_skew;
            int valid_off = older ? previous_have_off : have_off;
            event_pl_t event = { .master_boot_id = master_boot_id, .capture_seq = capture_seq, .end_seq = capture_seq,
                .sync_age_ms = sensor_sync_age_ms(valid_off, ref, ev_tick),
                .flags = sensor_health(valid_off, valid_skew, ref, ev_tick) };
            if (!capture_xtal) event.flags &= ~HEALTH_CLOCK_XTAL;
            event.ev_master_t = event.end_tick = ev_to_master_t(ev_tick, off, ref, skew, valid_skew);
            if (event.flags != HEALTH_EVENT_REQUIRED || pending_n >= SENSOR_EVENT_QUEUE_LEN || loss_pending) {
                if (event.flags != HEALTH_EVENT_REQUIRED) event.flags |= EVENT_TIME_UNKNOWN;
                sensor_note_loss(&loss, &loss_pending, &event);
                sat_inc_u16(&event_drop);
                continue;
            }
            sensor_pending_t *p = &pending[pending_head];
            p->event = event; p->event.ev_seq = ev_seq++;
            p->first_ms = p->retry_ms = board_millis();
            pending_head = (pending_head + 1u) % SENSOR_EVENT_QUEUE_LEN; pending_n++;
        }
        /* Checkpoint: periodic, or on request from a beacon (cp_pending) — only once our
         * own events are acked, so the checkpoint follows the capture it was asked for. */
        if (!pending_n && !loss_pending && timing_ready &&
            (cp_pending || (uint32_t)(board_millis() - checkpoint_ms) >= STATUS_PERIOD_S * 1000u)) {
            uint64_t at;
            uint32_t seq_at;
            if (capture_sensor_checkpoint(&at, &seq_at) &&
                sensor_health(have_off, skew_valid, sync_ref_tick, at) == HEALTH_EVENT_REQUIRED) {
                sensor_pending_t *p = &pending[pending_head];
                uint64_t master_at = ev_to_master_t(at, cur_off, sync_ref_tick, cur_skew, skew_valid);
                p->event = (event_pl_t){ .ev_seq = ev_seq++, .master_boot_id = master_boot_id,
                    .capture_seq = seq_at, .end_seq = seq_at, .ev_master_t = master_at, .end_tick = master_at,
                    .flags = HEALTH_EVENT_REQUIRED | EVENT_CHECKPOINT,
                    .sync_age_ms = sensor_sync_age_ms(have_off, sync_ref_tick, at) };
                p->first_ms = p->retry_ms = board_millis();
                pending_head = (pending_head + 1u) % SENSOR_EVENT_QUEUE_LEN; pending_n++;
                checkpoint_ms = board_millis();
                if (cp_pending) { cp_answered = cp_pending; cp_pending = 0; }
            }
        }
        if (pending_n) {
            uint32_t now_ms = board_millis();
            sensor_pending_t *p = &pending[pending_tail];
            if (!(p->event.flags & (EVENT_LOSS | EVENT_CHECKPOINT)) &&
                (uint32_t)(now_ms - p->first_ms) >= SENSOR_EVENT_MAX_AGE_MS) {
                sensor_note_loss(&loss, &loss_pending, &p->event);
                sat_inc_u16(&event_drop);
                pending_tail = (pending_tail + 1u) % SENSOR_EVENT_QUEUE_LEN; pending_n--;
            } else if ((int32_t)(now_ms - p->retry_ms) >= 0) {
                if (sensor_try_send_event(&p->event, my_id, master_boot_id, &from_master)) {
                    pending_tail = (pending_tail + 1u) % SENSOR_EVENT_QUEUE_LEN; pending_n--;
                } else p->retry_ms = board_millis() + SENSOR_EVENT_RETRY_MS + ((p->event.ev_seq * 7u + my_id * 11u) & 31u);
            }
        }

        /* STATUS: once per STATUS_PERIOD_S beacons, at a hash-chosen offset measured
         * FROM the beacon RxDone (prev_l_rx) so the ~46 ms transmit sits in the
         * guarded middle of an inter-beacon gap and is never on air when a beacon
         * arrives — the radio can't receive while it transmits, so the previous
         * absolute-phase scheme made us deaf to the beacons it overlapped (DESIGN
         * §2.8). The (id, period) hash picks which beacon in the period carries our
         * STATUS and the offset within its gap, so sensors self-assign with no master
         * coordination and re-hash each period; the CAD in sensor_send_status defers a
         * rare sensor-vs-sensor overlap. */
        if (have_off && have_prev && !sent_status &&
            (uint8_t)(prev_seq % STATUS_PERIOD_S) == (uint8_t)(my_id % STATUS_PERIOD_S)) {
            /* Fixed beacon phase (my_id % STATUS_PERIOD_S) → STATUS lands on the same
             * 1-of-5 beacons every period, so the interval is a regular ~5 s (no jitter,
             * never drifts toward the STALE window) — like the old fixed slot, but self-
             * assigned from the chip id with no slot number. Only the in-gap offset is
             * re-hashed per period, so two sensors that share a phase still don't collide
             * permanently (self-healing); the CAD in sensor_send_status defers the rare
             * same-period overlap, and a lost STATUS is loss-tolerant. */
            uint32_t period = (uint32_t)(prev_seq / STATUS_PERIOD_S);
            uint32_t within = STATUS_GAP_GUARD_MS + hash32(my_id, period) % STATUS_GAP_SPAN_MS;
            uint64_t status_tick = prev_l_rx + (uint64_t)within * TICKS_PER_MS;
            if (capture_now64() >= status_tick) {
                uint64_t status_now = capture_now64();
                uint16_t overflow = capture_sensor_overflow();
                uint16_t age = sensor_sync_age_ms(have_off, sync_ref_tick, status_now);
                uint8_t health = sensor_health(have_off, skew_valid, sync_ref_tick,
                                               status_now);
                sensor_send_status(my_id, &status_seq, cur_off, cur_skew, rx_miss,
                                   beacon_gap, age, overflow, event_drop, health);
                sent_status = 1;
            }
        }


    }
}

/* ===== master =========================================================== */

typedef struct {
    int      used;         /* registry entry in use */
    uint32_t id;           /* the sensor's 32-bit id (low word of its chip id) */
    int      seen;
    uint8_t  last_status_seq; int have_status; uint16_t status_miss;
    uint32_t last_status_ms;
    float    rssi, snr;
    int64_t  offset_tick;
    int32_t  skew_ppm;
    uint16_t rx_miss, beacon_gap;
    uint16_t sync_age_ms;
    uint16_t capture_overflow;
    uint16_t event_drop;
    uint8_t  health_flags;
    uint16_t batt_mv;
    int16_t  temp_c10;
    uint32_t last_lat_ms;
    int      last_state;
    sec_replay_t rx;   /* per-sensor replay window (EVENT + STATUS uplinks) */
    uint32_t sec_drop; /* authenticated-but-rejected packets from this sensor
                        * (replay / freshness / session-binding) — observability */
} node_stat_t;

static node_stat_t g_node[MAX_NODES];

/* Master-side AEAD verification failures (forgery / wrong key / corruption).
 * Unattributable (the node_id in a failed packet isn't authenticated), so it's a
 * single global counter, surfaced on the node-0 self-report D line. */
static uint32_t g_auth_drop;

static int node_count_seen(void)
{
    int c = 0;
    for (unsigned i = 0; i < MAX_NODES; i++) { if (g_node[i].used && g_node[i].seen) { c++; } }
    return c;
}

static int link_state_of(uint32_t now, const node_stat_t *s)
{
    if (!s->have_status) { return PU_STATE_LOST; }
    uint32_t age = now - s->last_status_ms;
    /* STATUS arrives every STATUS_PERIOD_S; with beacon-anchored STATUS it should
     * not be missed, so keep this tight — one missed STATUS already means something
     * is wrong and should surface, not be hidden behind a lenient window. */
    if (age <= 2u * STATUS_PERIOD_S * 1000u) { return PU_STATE_OK; }
    if (age <= 15000u) { return PU_STATE_STALE; }
    return PU_STATE_LOST;
}

/* Find this sender's registry entry, or claim one for it (auto-registration):
 * an unused entry, else one whose sensor has gone LOST. Returns NULL if all
 * MAX_NODES entries are live sensors. An entry claimed for a NEW id is wiped so
 * the newcomer isn't rejected by the previous occupant's replay window. */
static node_stat_t *node_find_or_add(uint32_t id)
{
    for (unsigned i = 0; i < MAX_NODES; i++) {
        if (g_node[i].used && g_node[i].id == id) { return &g_node[i]; }
    }
    int slot = -1;
    for (unsigned i = 0; i < MAX_NODES; i++) { if (!g_node[i].used) { slot = (int)i; break; } }
    if (slot < 0) {
        uint32_t now = board_millis();
        for (unsigned i = 0; i < MAX_NODES; i++) {
            if (link_state_of(now, &g_node[i]) == PU_STATE_LOST) { slot = (int)i; break; }
        }
    }
    if (slot < 0) { return NULL; } /* registry full of live sensors */
    node_stat_t fresh = {0};
    fresh.last_state = -1;
    fresh.used = 1;
    fresh.id = id;
    g_node[slot] = fresh;
    return &g_node[slot];
}

typedef struct {
    uint32_t node_id;
    uint32_t sensor_boot_id;
    event_pl_t event;
    float rssi, snr;
    uint32_t last_emit_ms;
} master_event_t;

static master_event_t g_event_queue[MASTER_EVENT_QUEUE_LEN];
static unsigned g_event_head;
static unsigned g_event_tail;
static unsigned g_event_count;
static uint16_t g_event_backpressure;

static int event_key_eq(const master_event_t *q, uint32_t node_id,
                        uint16_t ev_seq, uint64_t ev_master_t, uint32_t sensor_boot_id)
{
    return q->sensor_boot_id == sensor_boot_id && q->node_id == node_id && q->event.ev_seq == ev_seq &&
           q->event.ev_master_t == ev_master_t;
}

static int master_event_queued(uint32_t node_id, uint16_t ev_seq,
                               uint64_t ev_master_t, uint32_t sensor_boot_id)
{
    unsigned at = g_event_tail;
    for (unsigned i = 0; i < g_event_count; i++) {
        if (event_key_eq(&g_event_queue[at], node_id, ev_seq, ev_master_t, sensor_boot_id)) { return 1; }
        at = (at + 1u) % MASTER_EVENT_QUEUE_LEN;
    }
    return 0;
}

static int master_event_enqueue(uint32_t node_id, const event_pl_t *event,
                                uint32_t sensor_boot_id, float rssi, float snr)
{
    if (master_event_queued(node_id, event->ev_seq, event->ev_master_t, sensor_boot_id)) { return 1; }
    if (g_event_count >= MASTER_EVENT_QUEUE_LEN) {
        sat_inc_u16(&g_event_backpressure);
        return 0;
    }
    master_event_t *q = &g_event_queue[g_event_head];
    q->node_id = node_id;
    q->sensor_boot_id = sensor_boot_id;
    q->event = *event;
    q->rssi = rssi;
    q->snr = snr;
    q->last_emit_ms = board_millis() - MASTER_USB_RETRY_MS;
    g_event_head = (g_event_head + 1u) % MASTER_EVENT_QUEUE_LEN;
    g_event_count++;
    return 1;
}

static void master_event_host_ack(uint32_t node_id, uint16_t ev_seq,
                                  uint64_t ev_master_t, uint32_t sensor_boot_id, uint32_t master_boot_id)
{
    if (!g_event_count) { return; }
    master_event_t *q = &g_event_queue[g_event_tail];
    if (q->event.master_boot_id != master_boot_id || !event_key_eq(q, node_id, ev_seq, ev_master_t, sensor_boot_id)) { return; }
    g_event_tail = (g_event_tail + 1u) % MASTER_EVENT_QUEUE_LEN;
    g_event_count--;
}

static void master_event_pump(void)
{
    if (!g_event_count) { return; }
    master_event_t *q = &g_event_queue[g_event_tail];
    uint32_t now = board_millis();
    if ((uint32_t)(now - q->last_emit_ms) < MASTER_USB_RETRY_MS) { return; }
    q->last_emit_ms = now;
    pu_emit_event(q->node_id, &q->event, q->sensor_boot_id, q->rssi, q->snr);
}

typedef struct {
    int started;
    int valid;
    uint16_t last_frame;
    uint32_t frames;
    uint64_t start_tick;
    uint32_t last_sof_ms;
    int32_t ppm;
} usb_clock_t;

static void usb_clock_poll(usb_clock_t *clock)
{
    uint64_t tick;
    uint16_t frame;
    uint32_t now = board_millis();
    if (!capture_usb_sof_sample(&tick, &frame)) {
        if ((uint32_t)(now - clock->last_sof_ms) > 100u) {
            clock->started = 0;
            clock->valid = 0;
        }
        return;
    }
    frame &= 0x07ffu;
    if (!clock->started) {
        clock->started = 1;
        clock->last_frame = frame;
        clock->frames = 0;
        clock->start_tick = tick;
        clock->last_sof_ms = now;
        return;
    }
    uint16_t delta = (uint16_t)((frame - clock->last_frame) & 0x07ffu);
    if (delta == 0u) {
        if ((uint32_t)(now - clock->last_sof_ms) > 100u) {
            clock->started = 0;
            clock->valid = 0;
        }
        return;
    }
    if (delta > USB_CLOCK_MAX_GAP_FRAMES) {
        clock->started = 0;
        clock->valid = 0;
        return;
    }
    clock->last_frame = frame;
    clock->last_sof_ms = now;
    clock->frames += delta;
    if (clock->frames >= USB_CLOCK_WINDOW_FRAMES) {
        uint64_t expected = (uint64_t)clock->frames * TICKS_PER_MS;
        int64_t error = (int64_t)(tick - clock->start_tick) - (int64_t)expected;
        int64_t expected_i = (int64_t)expected;
        int64_t ppm = (error / expected_i) * 1000000 +
                      (error % expected_i) * 1000000 / expected_i;
        clock->ppm = (int32_t)(ppm > INT32_MAX ? INT32_MAX :
                               (ppm < INT32_MIN ? INT32_MIN : ppm));
        clock->valid = 1;
        clock->frames = 0;
        clock->start_tick = tick;
    }
}

static void master_emit_diag(const node_stat_t *s, int state, uint32_t now)
{
    pu_emit_diag(s->id, 0,
                 state, s->offset_tick, s->skew_ppm,
                 s->rx_miss, s->beacon_gap,
                 (uint32_t)(now - s->last_status_ms),
                 s->rssi, s->snr, s->last_lat_ms,
                 s->temp_c10, s->batt_mv,
                 s->sec_drop, 1,
                 !!(s->health_flags & HEALTH_SYNC_VALID),
                 !!(s->health_flags & HEALTH_SKEW_VALID),
                 !!(s->health_flags & HEALTH_CLOCK_XTAL), s->sync_age_ms,
                 s->capture_overflow, s->event_drop, 0, 0, 0, 0, s->rx.boot_id, sec_boot_id());
}

// A clock-source fault ends the timebase session. Its reliable marker precedes
// new-session data; sensors must acquire fresh offset/skew after recovery.
static int master_clock_check(void)
{
    static int faulted, reported;
    static uint16_t fault_seq;
    static event_pl_t fault;
    if (!board_hfclk_xtal() && !faulted) {
        uint64_t at = capture_now64();
        fault = (event_pl_t){ .ev_seq = fault_seq++, .master_boot_id = sec_boot_id(),
            .ev_master_t = at, .end_tick = at, .flags = EVENT_LOSS };
        faulted = 1; reported = 0;
    }
    if (!faulted) return 1;
    if (!reported) reported = master_event_enqueue(NODE_MASTER, &fault, fault.master_boot_id, 0, 0);
    if (reported && board_hfclk_xtal()) {
        sec_init();
        faulted = 0;
        return 1;
    }
    return 0;
}

/* Checkpoint on demand (config.h CP_REQ_BEACONS): the next g_cp_left beacons carry
 * g_cp_id so every synced sensor checkpoints once. Raised when a fresh sensor
 * capture/loss event is queued for the host and when the PC sends `CP`. */
static uint8_t g_cp_id;
static unsigned g_cp_left;

static void master_request_checkpoint(void)
{
    g_cp_id = (uint8_t)(g_cp_id % 255u + 1u); /* 1..255, never the "no request" 0 */
    g_cp_left = CP_REQ_BEACONS;
}

static void run_master(int st)
{
    for (unsigned i = 0; i < MAX_NODES; i++) { g_node[i].last_state = -1; }
    uint8_t seq = 0;
    uint64_t m_tx_last = 0;
    uint32_t last = board_millis();
    uint8_t buf[WIRE_MAX];
    usb_clock_t usb_clock = {0};

    pu_emit_identity(node_devid_hi(), node_devid_lo());

    for (;;) {
        usb_task();

        int c;
        while ((c = usb_read_byte()) >= 0) {
            switch (pu_feed(c)) {
            case PU_CMD_ID:
                pu_emit_identity(node_devid_hi(), node_devid_lo());
                pu_emit_ack("ID");
                break;
            case PU_CMD_STATUS: {
                uint32_t now = board_millis();
                pu_emit_heartbeat((st == 0) ? capture_now64() : 0, now, seq, node_count_seen());
                for (unsigned i = 0; i < MAX_NODES; i++) {
                    if (g_node[i].used && g_node[i].seen) { master_emit_diag(&g_node[i], link_state_of(now, &g_node[i]), now); }
                }
                pu_emit_diag(0, 1, st == 0 ? PU_STATE_OK : PU_STATE_LOST,
                             0, 0, 0, 0, 0, 0.0f, 0.0f, 0,
                             meas_temp_c10(), meas_vddh_mv(), g_auth_drop,
                             sec_provisioned(), st == 0, st == 0,
                             board_hfclk_xtal(), st == 0 ? 0 : UINT16_MAX,
                             0, 0, (uint16_t)g_event_count, g_event_backpressure,
                             usb_clock.valid, usb_clock.ppm, 0, sec_boot_id());
                if (st == 0) {
                    gps_report_t g;
                    gps_report(&g);
                    pu_emit_pps(g.pps_tick, g.utc_s, g.ppb, g.pps_valid, g.fix, g.sats, g.span_s);
                }
                pu_emit_ack("STATUS");
                break;
            }
            case PU_CMD_PING:
                pu_emit_ack("PING");
                break;
            case PU_CMD_SETKEY:
                provision_key();
                break;
            case PU_CMD_EVENT_ACK:
                master_event_host_ack(pu_event_ack_node(), pu_event_ack_seq(),
                                      pu_event_ack_tick(), pu_event_ack_sensor_boot(), pu_event_ack_boot());
                break;
            case PU_CMD_CLOCK:
                if (st != 0) { pu_emit_err("clock"); break; }
                pu_emit_clock(pu_clock_token(), capture_now64(), sec_boot_id());
                break;
            case PU_CMD_CHECKPOINT:
                if (st != 0) { pu_emit_err("clock"); break; } /* no beacons without a timebase */
                master_request_checkpoint();
                pu_emit_ack("CP");
                break;
            case PU_CMD_BAD:
                pu_emit_err("badcmd");
                break;
            default:
                break;
            }
        }
        master_event_pump();
#if defined(NODE_DEBUG)
        debug_id("master");
#endif
        if (st != 0) {
            uint32_t now = board_millis();
            if ((uint32_t)(now - last) >= 1000u) {
                last = now;
                pu_emit_heartbeat(0, now, seq++, node_count_seen());
                if ((uint8_t)(seq % STATUS_PERIOD_S) == 0u) {
                    pu_emit_diag(0, 1, PU_STATE_LOST, 0, 0, 0, 0, 0,
                                 0.0f, 0.0f, 0, meas_temp_c10(), meas_vddh_mv(),
                                 g_auth_drop, sec_provisioned(), 0, 0,
                                 board_hfclk_xtal(), UINT16_MAX, 0, 0,
                                 (uint16_t)g_event_count, g_event_backpressure, 0, 0, 0, sec_boot_id());
                }
            }
            continue;
        }
        capture_now64();
        if (!master_clock_check()) { master_event_pump(); continue; }
        usb_clock_poll(&usb_clock);
        gps_poll();

        uint32_t now = board_millis();
        if ((uint32_t)(now - last) >= 1000u) {
            last = now;
            /* No key yet → sec_seal refuses, so no beacon goes out; tell the PC. */
            if (!sec_provisioned() && (seq % 5u) == 0u) { pu_emit_err("noprov"); }
            beacon_pl_t b;
            b.seq = seq;
            b.m_tx_prev = m_tx_last;
            b.cp_req = g_cp_left ? g_cp_id : 0u;
            if (g_cp_left) { g_cp_left--; }
            uint8_t tx[WIRE_BEACON];
            int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_BEACON, NODE_MASTER, &b, sizeof(b));
            /* Best-effort LBT before the beacon (KR920): sense the channel and, if
             * busy, re-sense up to BEACON_LBT_TRIES times spaced BEACON_LBT_GAP_MS
             * apart so an in-flight peer STATUS (~46 ms) finishes — then transmit
             * regardless. The beacon is the sync anchor with no retransmit, so it is
             * never dropped (a skip makes every sensor miss it). The wait does not
             * affect sync accuracy: sync rides the actual TxDone capture below, not
             * the nominal beacon instant. */
            if (wlen > 0) {
                for (unsigned i = 0; i < BEACON_LBT_TRIES && !radio_lbt_clear(); i++) {
                    board_delay_ms(BEACON_LBT_GAP_MS);
                }
                radio_transmit(tx, wlen);
                board_led_toggle(); /* heartbeat: blink on each beacon TX, mirroring the sensor's blink on beacon RX */
                uint64_t cap = 0;
                if (capture_dio1_get(&cap)) { m_tx_last = cap; }
            }
            radio_start_rx();

            pu_emit_heartbeat(capture_now64(), now, seq, node_count_seen());
            seq++;

            for (unsigned i = 0; i < MAX_NODES; i++) {
                if (!g_node[i].used || !g_node[i].seen) { continue; }
                int ls = link_state_of(now, &g_node[i]);
                if (ls != g_node[i].last_state) {
                    g_node[i].last_state = ls;
                    master_emit_diag(&g_node[i], ls, now);
                }
            }

            /* Master self-diag (node 0) every STATUS_PERIOD_S beacons: its own die
             * temp + charge-rail voltage. last_seen=0 (just measured); LoRa-only
             * fields are 0 since they don't apply to the master itself. The
             * sec_drop slot carries the global AEAD-failure count, and the
             * provisioned flag rides here so the server sees both. */
            if ((uint8_t)(seq % STATUS_PERIOD_S) == 0u) {
                pu_emit_diag(0, 1, PU_STATE_OK, 0, 0, 0, 0, 0, 0.0f, 0.0f, 0,
                             meas_temp_c10(), meas_vddh_mv(),
                             g_auth_drop, sec_provisioned(), 1, 1,
                             board_hfclk_xtal(), 0, 0, 0,
                             (uint16_t)g_event_count, g_event_backpressure,
                             usb_clock.valid, usb_clock.ppm, 0, sec_boot_id());
            }
            continue;
        }

        /* GPS report (~1 Hz) — never in the beacon iteration above, so it cannot
         * share a CDC TX FIFO window with the H/D burst (usb_write is all-or-nothing). */
        if (gps_report_due()) {
            gps_report_t g;
            gps_report(&g);
            pu_emit_pps(g.pps_tick, g.utc_s, g.ppb, g.pps_valid, g.fix, g.sats, g.span_s);
        }

        float rssi = 0, snr = 0;
        int n = radio_receive_q(buf, sizeof(buf), &rssi, &snr);
        if (n <= 0) { continue; }
        uint8_t type = SEC_VT_TYPE(buf[0]);

        if (type == PKT_TYPE_EVENT && n >= WIRE_EVENT) {
            sec_meta_t m;
            event_pl_t e;
            if (sec_unseal(buf, n, &m, &e, sizeof(e)) != 0) { g_auth_drop++; continue; }
            /* Identify (auto-register) the sender by its id, then validate. */
            node_stat_t *ns = node_find_or_add(m.node_id);
            if (!ns) { continue; } /* registry full of live sensors */
            /* Session binding: the event must name THIS master's current boot_id.
             * An event captured under a previous master power-cycle names the old
             * session and is dropped — closing cross-reboot EVENT replay
             * (DESIGN §2.11). A sensor learns the current id from live beacons, so
             * after a master swap its events re-bind as soon as it re-syncs. */
            if (e.master_boot_id != sec_boot_id()) { ns->sec_drop++; continue; }
            /* Queue capacity is checked before advancing the replay window. If
             * the queue is full we deliberately withhold the radio ACK and let
             * a freshly sealed retry arrive after the host drains capacity. */
            if (!master_event_queued(m.node_id, e.ev_seq, e.ev_master_t, m.boot_id) &&
                g_event_count >= MASTER_EVENT_QUEUE_LEN) {
                sat_inc_u16(&g_event_backpressure);
                continue;
            }
            if (!sec_replay(&ns->rx, m.boot_id, m.ctr)) { ns->sec_drop++; continue; }
            if (!(e.flags & (EVENT_LOSS | EVENT_CHECKPOINT)) &&
                ((e.flags & HEALTH_EVENT_REQUIRED) != HEALTH_EVENT_REQUIRED || e.sync_age_ms > SYNC_TTL_MS)) {
                ns->sec_drop++;
                continue;
            }
            /* Freshness backstop: reject a timestamp too far in the past (stale
             * replay) or implausibly in the future. Asymmetric — a real event is
             * at most a few ms ahead of master time (sync error). dt = now - ev. */
            uint64_t now_t = capture_now64();
            int64_t dt = (int64_t)now_t - (int64_t)e.ev_master_t;
            if (!(e.flags & (EVENT_LOSS | EVENT_CHECKPOINT)) &&
                (dt > (int64_t)EVENT_FRESH_MS * (int64_t)TICKS_PER_MS ||
                 dt < -((int64_t)EVENT_FUTURE_MS * (int64_t)TICKS_PER_MS))) {
                ns->sec_drop++; continue;
            }
            ns->seen = 1;
            ns->rssi = rssi;
            ns->snr = snr;
            ns->last_lat_ms = (uint32_t)((dt < 0 ? 0 : dt) / TICKS_PER_MS);
            /* The radio ACK is legal only after the event is resident in the
             * master's host-acknowledged queue. USB output is retried from that
             * queue until the server confirms the exact event key. */
            int fresh = !master_event_queued(m.node_id, e.ev_seq, e.ev_master_t, m.boot_id);
            if (!master_event_enqueue(ns->id, &e, m.boot_id, rssi, snr)) { continue; }
            /* A new capture/loss asks every sensor to checkpoint on the next beacons,
             * so the PC can confirm the other sensors' silence through this event's
             * tick within ~1 s (a retransmit of a queued event does not re-ask). */
            if (fresh && !(e.flags & EVENT_CHECKPOINT)) { master_request_checkpoint(); }
            ack_pl_t a;
            a.node_id = m.node_id;
            a.ev_seq = e.ev_seq;
            a.sensor_boot_id = m.boot_id;
            a.ev_master_t = e.ev_master_t;
            uint8_t tx[WIRE_ACK];
            int wlen = sec_seal(tx, sizeof(tx), PKT_TYPE_ACK, NODE_MASTER, &a, sizeof(a));
            /* LBT before the ACK (KR920); if busy, skip — the sensor retransmits
             * and we re-ACK. */
            if (wlen > 0 && radio_lbt_clear()) { radio_transmit(tx, wlen); }
            radio_start_rx();
        } else if (type == PKT_TYPE_STATUS && n >= WIRE_STATUS) {
            sec_meta_t m;
            status_pl_t s;
            if (sec_unseal(buf, n, &m, &s, sizeof(s)) != 0) { g_auth_drop++; continue; }
            node_stat_t *ns = node_find_or_add(m.node_id);
            if (!ns) { continue; } /* registry full of live sensors */
            if (!sec_replay(&ns->rx, m.boot_id, m.ctr)) { ns->sec_drop++; continue; }
            if (ns->have_status) {
                uint8_t d = (uint8_t)(s.seq - ns->last_status_seq);
                if (d > 1u) { ns->status_miss += (uint16_t)(d - 1u); }
            }
            ns->have_status = 1;
            ns->last_status_seq = s.seq;
            ns->seen = 1;
            ns->rssi = rssi;
            ns->snr = snr;
            ns->last_status_ms = board_millis();
            ns->offset_tick = s.offset_tick;
            ns->skew_ppm = s.skew_ppm;
            ns->rx_miss = s.rx_miss;
            ns->beacon_gap = s.beacon_gap;
            ns->sync_age_ms = s.sync_age_ms;
            ns->capture_overflow = s.capture_overflow;
            ns->event_drop = s.event_drop;
            ns->health_flags = s.flags;
            ns->batt_mv = s.batt_mv;
            ns->temp_c10 = s.temp_c10;
            int ls = link_state_of(board_millis(), ns);
            ns->last_state = ls;
            master_emit_diag(ns, ls, board_millis());
        }
    }
}

int main(void)
{
    board_init();
    node_init();
    sec_init(); /* seed boot_id (RNG) + reset tx counter before any sealed TX */
    usb_init();

    /* Role by USB (DESIGN §8): master if a PC host enumerates us within the
     * settle window, else sensor. Decide once; a role change requires reboot. */
    int master = role_decide_master();

    /* USB may have started HFXO while the role was being resolved. Inspect the
     * actual source only after that window, then fail closed: TIMER1/radio work
     * is never started on HFINT. Provisioning and RC diagnostics remain usable. */
    int st = -1;
    if (board_hfclk_xtal()) {
        st = radio_begin(node_freq_mhz());
        if (st == 0) {
            capture_init();
            if (master) {
                capture_usb_sof_enable();
                capture_pps_enable(); /* SENSOR capture channel -> GPS PPS */
                gps_init();
            }
        }
    }

    if (master) {
        run_master(st);
    } else {
        run_sensor(st);
    }
}
