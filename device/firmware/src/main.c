/* One master and up to MAX_NODES sensors on one channel and one timebase; one
 * binary, role decided at boot from USB (DESIGN §8).
 *
 * MASTER = the board a PC enumerates over USB-CDC: beacons, collects sensor
 * records into the host queue and speaks the FSK-WL line protocol (proto_usb.h).
 * SENSOR = any other board: syncs to beacons, captures SENSOR edges in hardware
 * and uplinks them in its slot (mac_sensor.h).
 */
#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "capture.h"
#include "config.h"
#include "errlog.h"
#include "fault.h"
#include "gpio.h"
#include "gps.h"
#include "keystore.h"
#include "mac_master.h"
#include "mac_sensor.h"
#include "master_queue.h"
#include "meas.h"
#include "node_id.h"
#include "power.h"
#include "proto_usb.h"
#include "radio.h"
#include "secure.h"
#include "usb.h"
#include "nrf.h"

/* Pump USB up to ROLE_SETTLE_MS for a PC to enumerate us: master when one does.
 * Decided once; a host that later suspends the bus does not change it. */
static int role_decide_master(void)
{
    uint32_t t0 = board_millis();
    for (;;) {
        usb_task();
        if (usb_host_present()) { return 1; }
        if ((uint32_t)(board_millis() - t0) >= ROLE_SETTLE_MS) { return 0; }
    }
}

/* `K <64-hex>` on any board: write the fleet key and use it at once. There is no
 * read-back command. */
static void provision_key(void)
{
    if (keystore_write(pu_setkey()) == 0) {
        sec_reload();
        pu_emit_ack("K");
    } else {
        pu_emit_err("keyfail");
    }
}

static void service(uint32_t now)
{
    usb_task();
    pu_service(now);
    fault_service(now);
    board_hfxo_service();
}

/* ===== sensor ============================================================ */

static mac_sensor_t g_sensor;

static void run_sensor(int st)
{
    if (st == 0) { mac_sensor_init(&g_sensor, node_sender_id(), fault_reset_reason()); }
    uint32_t last_blink = board_millis();
    for (;;) {
        uint32_t now = board_millis();
        service(now);
        int c;
        while ((c = usb_read_byte()) >= 0) {
            switch (pu_feed(c)) {
            case PU_CMD_SETKEY: provision_key(); break;
            case PU_CMD_ID:
                pu_emit_identity(node_devid_hi(), node_devid_lo(), 0, fault_reset_reason());
                pu_emit_ack("ID");
                break;
            case PU_CMD_PING: pu_emit_ack("PING"); break;
            case PU_CMD_BAD: pu_emit_err("badcmd"); break;
            default: break;
            }
        }
        if (st != 0) { /* no radio / no crystal: provisioning only */
            if ((uint32_t)(now - last_blink) >= 1000u) { last_blink = now; board_led_toggle(); }
            continue;
        }
        mac_sensor_step(&g_sensor);
    }
}

/* ===== master ============================================================ */

static mq_t g_queue;
static mac_master_t g_mac;
static uint32_t g_hfxo_stops; /* board_hfxo_stops() already handled */

/* A master HFXO fault ends the timebase session: its marker is queued ahead of
 * any record of the new session, which starts once the crystal is back. A stop
 * whose restart already finished counts too. */
static int master_clock_check(void)
{
    static int faulted, reported;
    static uint64_t at;
    static uint32_t boot;
    uint32_t stops = board_hfxo_stops();
    int stopped = stops != g_hfxo_stops;
    g_hfxo_stops = stops;
    if ((!board_hfclk_xtal() || stopped) && !faulted) {
        at = capture_now64();
        boot = sec_boot_id();
        faulted = 1;
        reported = 0;
    }
    if (!faulted) { return 1; }
    if (!reported) { reported = mq_push_timebase_end(&g_queue, boot, at); }
    if (reported && board_hfclk_xtal()) {
        sec_init();
        mac_master_session(&g_mac);
        faulted = 0;
        return 1;
    }
    return 0;
}

/* Node 0 self-report: die temperature, charge rail, security and queue counters. */
static void emit_self(int timebase_ok)
{
    pu_diag_t d = {0};
    d.is_master = 1;
    d.state = timebase_ok ? PU_STATE_OK : PU_STATE_LOST;
    d.temp_c10 = meas_temp_c10();
    d.batt_mv = meas_vddh_mv();
    d.sec_drop = g_mac.auth_drop;
    d.provisioned = sec_provisioned();
    d.health = (uint8_t)((timebase_ok ? (HEALTH_SYNC_VALID | HEALTH_SKEW_VALID) : 0u) |
                         (board_hfclk_xtal() ? HEALTH_CLOCK_XTAL : 0u));
    d.sync_age_ms = timebase_ok ? 0 : UINT16_MAX;
    d.queue_depth = (uint16_t)mq_depth(&g_queue);
    d.queue_overflow = g_queue.overflow;
    d.err_flags = el_flags();
    d.ver_drop = g_mac.ver_drop;
    d.tx_drop = pu_tx_drop();
    d.reset_reason = fault_reset_reason();
    d.master_boot_id = sec_boot_id();
    pu_emit_diag(&d);
}

static void emit_pps(void)
{
    gps_report_t g;
    gps_report(&g);
    pu_emit_pps(g.pps_tick, g.utc_s, g.ppb, g.pps_valid, g.fix, g.sats, g.span_s, g.seg, g.n);
}

/* USB unplugged: stop every radio activity until VBUS returns, then reboot
 * into a new session (W1). A battery-less master simply loses power. */
static void master_stopped(mp_state_t *power, int radio_up)
{
    if (radio_up) { radio_standby(); }
    board_led_off();
    uint32_t t0 = board_millis();
    for (;;) {
        usb_task();
        if (mp_step(power, usb_vbus_present()) == MP_RESET) { NVIC_SystemReset(); }
        gpio_write(PIN_LED_STATUS, (board_millis() - t0) % 2000u < 100u); /* short flash every 2 s */
    }
}

static void run_master(int st)
{
    mq_init(&g_queue);
    g_hfxo_stops = board_hfxo_stops();
    if (st == 0) { mac_master_init(&g_mac, &g_queue); }
    pu_emit_identity(node_devid_hi(), node_devid_lo(), 1, fault_reset_reason());
    uint32_t pc, lr, cfsr;
    const char *cause;
    if (fault_take_report(&pc, &lr, &cause, &cfsr)) { pu_emit_fault(pc, lr, cause, cfsr); }

    mp_state_t power = MP_RUN;
    uint32_t last = board_millis();
    uint8_t idle_seq = 0;
    for (;;) {
        uint32_t now = board_millis();
        service(now);
        if (mp_step(&power, usb_vbus_present()) == MP_STOP) { master_stopped(&power, st == 0); }
        el_report(now);

        int c;
        while ((c = usb_read_byte()) >= 0) {
            switch (pu_feed(c)) {
            case PU_CMD_ID:
                pu_emit_identity(node_devid_hi(), node_devid_lo(), 1, fault_reset_reason());
                pu_emit_ack("ID");
                break;
            case PU_CMD_STATUS:
                pu_emit_heartbeat(st == 0 ? capture_now64() : 0, now,
                                  st == 0 ? (uint8_t)(g_mac.seq - 1u) : idle_seq,
                                  st == 0 ? (int)reg_count(&g_mac.reg) : 0);
                if (st == 0) { mac_master_report(&g_mac, 0); }
                emit_self(st == 0);
                if (st == 0) { emit_pps(); }
                pu_emit_ack("STATUS");
                break;
            case PU_CMD_PING:
                pu_emit_ack("PING");
                break;
            case PU_CMD_SETKEY:
                provision_key();
                break;
            case PU_CMD_EVENT_ACK:
                (void)mq_ack(&g_queue, pu_ack_hseq(), pu_ack_boot(), pu_ack_crc());
                break;
            case PU_CMD_CLOCK:
                if (st != 0) { pu_emit_err("clock"); break; }
                pu_emit_clock(pu_clock_token(), capture_now64(), sec_boot_id());
                break;
            case PU_CMD_CHECKPOINT:
                if (st != 0) { pu_emit_err("clock"); break; } /* no beacons without a timebase */
                mac_master_checkpoint(&g_mac);
                pu_emit_ack("CP");
                break;
            case PU_CMD_BAD:
                pu_emit_err("badcmd");
                break;
            default:
                break;
            }
        }
        mq_pump(&g_queue, now);

        if (st != 0) {
            if ((uint32_t)(now - last) >= 1000u) {
                last = now;
                pu_emit_heartbeat(0, now, idle_seq++, 0);
                if ((uint8_t)(idle_seq % CHECKPOINT_PERIOD_BEACONS) == 0u) { emit_self(0); }
            }
            continue;
        }
        if (!master_clock_check()) { continue; }
        gps_poll();
        if (gps_report_due()) { emit_pps(); }

        if (mac_master_step(&g_mac)) {
            uint8_t sent = (uint8_t)(g_mac.seq - 1u);
            /* Without a key sec_seal refuses: no beacon goes out; tell the PC. */
            if (!sec_provisioned() && (sent % 5u) == 0u) { pu_emit_err("noprov"); }
            pu_emit_heartbeat(capture_now64(), now, sent, (int)reg_count(&g_mac.reg));
            mac_master_report(&g_mac, 1);
            if ((uint8_t)(g_mac.seq % CHECKPOINT_PERIOD_BEACONS) == 0u) { emit_self(1); }
        }
    }
}

int main(void)
{
    fault_boot(); /* may halt after repeated fault reboots */
    board_init();
    node_init();
    sec_init();   /* boot_id + key before any sealed TX */
    usb_init();

    int master = role_decide_master();

    /* USB may have started HFXO while the role was being resolved; check the
     * real source only now and fail closed: no radio or TIMER1 work on HFINT.
     * Provisioning over USB stays available either way. */
    int st = -1;
    if (board_hfclk_xtal()) {
        st = radio_begin();
        if (st == 0) {
            capture_init();
            if (master) {
                capture_pps_enable();
                gps_init();
            } else {
                capture_sensor_enable();
            }
        }
    }

    if (master) {
        run_master(st);
    } else {
        run_sensor(st);
    }
}
