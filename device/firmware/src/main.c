/* One master and up to MAX_NODES sensors, one channel/timebase; one binary, role from USB at boot.
 * MASTER (a PC enumerates it): beacons, queues sensor records, speaks FSK-WL lines (proto_usb.h).
 * SENSOR: syncs to beacons, hardware-captures SENSOR edges, uplinks in its slots (mac_sensor.h). */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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
#include "radio_cfg.h"
#include "secure.h"
#include "retry.h"
#include "usb.h"
#include "nrf.h"

/* Master iff a PC enumerates us in ROLE_SETTLE_MS; decided once (a later bus suspend keeps it). */
static int role_decide_master(void)
{
    uint32_t t0 = board_millis();
    for (;;) {
        usb_task();
        if (usb_host_present()) { return 1; }
        if ((uint32_t)(board_millis() - t0) >= ROLE_SETTLE_MS) { return 0; }
    }
}

/* USB-only readback of the stored fleet key, independent of radio readiness. */
static void read_key(void)
{
    uint8_t key[KEYSTORE_KEY_LEN];
    pu_emit_key(keystore_load(key) ? key : NULL);
}

/* `K <64-hex>` on any board: write the fleet key and use it at once. */
static int provision_key(void)
{
    if (keystore_write(pu_setkey()) == 0) {
        sec_reload();
        pu_emit_ack("K");
        return 1;
    } else {
        pu_emit_err("keyfail");
    }
    return 0;
}

/* `R <freq_khz> <power_dbm> <lbt>` on any board: store and use now (a down radio takes them at its
 * next bring-up). The I line shows the stored settings; only a running receiver earns A R OK. */
static void provision_radio(int master, int radio_up)
{
    const radio_cfg_t *c = pu_radio_cfg();
    if (radio_cfg_write(c) != 0) {
        pu_emit_err("cfgfail");
        return;
    }
    radio_set_config(c);
    int applied = radio_up && radio_begin() == 0 && radio_start_rx() == 0;
    pu_emit_identity(node_devid_hi(), node_devid_lo(), master, fault_reset_reason(), radio_config());
    if (applied) { pu_emit_ack("R"); }
    else { pu_emit_err(radio_up ? "cfgapply" : "cfgpending"); }
}

static void service(uint32_t now)
{
    usb_task();
    pu_service(now);
    fault_service(now);
    board_hfxo_service();
    sec_service(now);
}

/* ===== bring-up ========================================================== */

/* Radio, TIMER1, role captures; 0 when up. Fail closed: no radio or TIMER1 work on HFINT. */
static int bring_up(int master)
{
    if (!board_hfclk_xtal()) { return -1; }
    int st = radio_begin();
    if (st != 0) { return st; }
    st = radio_start_rx();
    if (st != 0) { return st; }
    /* Start captures only after the last fallible step, so a retry cannot reset live counters. */
    capture_init();
    if (master) {
        capture_pps_enable();
        gps_init();
    } else {
        capture_sensor_enable();
    }
    return 0;
}

/* 1 once a due retry has brought the board up. */
static int retry_bring_up(retry_t *r, int master, uint32_t now)
{
    if (!retry_due(r, now)) { return 0; }
    if (!board_hfclk_xtal()) {
        retry_later(r, now, RADIO_RESET_BACKOFF_MAX_MS);
        return 0;
    }
    el_radio_recovery_begin(RADIO_RECOVERY_BOOT);
    int ok = bring_up(master) == 0;
    el_radio_recovery_end(ok);
    if (ok) { return 1; }
    retry_later(r, board_millis(), RADIO_RESET_BACKOFF_MAX_MS);
    return 0;
}

/* ===== sensor ============================================================ */

static mac_sensor_t g_sensor;

static void run_sensor(int st)
{
    if (st == 0) { mac_sensor_init(&g_sensor, node_sender_id(), fault_reset_reason()); }
    retry_t retry;
    retry_init(&retry, board_millis(), RADIO_BOOT_RETRY_MS);
    uint32_t last_blink = board_millis();
    for (;;) {
        uint32_t now = board_millis();
        service(now);
        int c;
        while ((c = usb_read_byte()) >= 0) {
            switch (pu_feed(c)) {
            case PU_CMD_SETKEY:
                if (provision_key()) {
                    memset(g_sensor.data_key, 0, sizeof(g_sensor.data_key));
                    memset(&g_sensor.join, 0, sizeof(g_sensor.join));
                    g_sensor.link_id = 0; g_sensor.tx_slots = 0;
                }
                break;
            case PU_CMD_GETKEY: read_key(); break;
            case PU_CMD_RADIO: provision_radio(0, st == 0); break;
            case PU_CMD_ID:
                pu_emit_identity(node_devid_hi(), node_devid_lo(), 0, fault_reset_reason(), radio_config());
                pu_emit_ack("ID");
                break;
            case PU_CMD_PING: pu_emit_ack("PING"); break;
            case PU_CMD_BAD: pu_emit_err("badcmd"); break;
            default: break;
            }
        }
        if (st != 0) { /* no radio / no crystal: provisioning plus periodic retry */
            if (retry_bring_up(&retry, 0, now)) {
                st = 0;
                mac_sensor_init(&g_sensor, node_sender_id(), fault_reset_reason());
                continue;
            }
            if ((uint32_t)(now - last_blink) >= 1000u) { last_blink = now; board_led_toggle(); }
            continue;
        }
        mac_sensor_step(&g_sensor);
    }
}

/* ===== master ============================================================ */

static mq_t g_queue;
static mac_master_t g_mac;
static int g_rekey;
static uint32_t g_hfxo_stops; /* board_hfxo_stops() already handled */
static uint64_t g_reported_boot;
static uint32_t g_reported_usb_epoch;

/* HFXO fault (even a restarted stop) ends the timebase session; its marker precedes new records. */
static int master_clock_check(void)
{
    static int faulted, reported;
    static uint64_t at;
    static uint64_t boot;
    uint32_t stops = board_hfxo_stops();
    int stopped = stops != g_hfxo_stops;
    g_hfxo_stops = stops;
    if ((!board_hfclk_xtal() || stopped || sec_counter_exhausted() || g_rekey) && !faulted) {
        at = capture_now64();
        boot = sec_boot_id();
        faulted = 1;
        reported = 0;
    }
    if (!faulted) { return sec_boot_ready(); }
    /* A full host queue defers the marker; retrying every pass is not an overflow. */
    if (!reported && !mq_full(&g_queue)) { reported = mq_push_timebase_end(&g_queue, boot, at); }
    if (reported && board_hfclk_xtal()) {
        g_rekey = 0;
        gps_discard_pending();
        sec_init();
        mac_master_session(&g_mac);
        faulted = 0;
        return sec_boot_ready();
    }
    return 0;
}

/* Node 0 self-report: die temperature, charge rail, security and queue counters. */
static int emit_self(int timebase_ok)
{
    timebase_ok = timebase_ok && sec_ready() && !radio_needs_reset();
    pu_diag_t d = {0};
    d.is_master = 1;
    d.diag_valid = 1; /* self diagnostics are observed locally, even when unhealthy */
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
    /* Queue full is current state; refused-enqueue totals stay in overflow and X queue_full. */
    d.err_flags = (uint16_t)(el_flags() & (uint16_t)~ERR_QUEUE_FULL);
    if (mq_full(&g_queue)) { d.err_flags |= ERR_QUEUE_FULL; }
    d.recovery = el_radio_recovery();
    d.ver_drop = g_mac.ver_drop;
    d.tx_drop = pu_tx_drop();
    d.reset_reason = fault_reset_reason();
    d.master_boot_id = sec_boot_id();
    if (!pu_emit_diag(&d)) { return 0; }
    g_reported_boot = d.master_boot_id;
    g_reported_usb_epoch = usb_console_epoch();
    return 1;
}

static void emit_pps(void)
{
    if (!usb_console_ready() || !gps_report_due()) { return; } /* includes ?STATUS */
    gps_report_t g;
    /* Best effort: consumed even if unsent; a full USB buffer costs at most one try per sample. */
    gps_report(&g);
    /* P has no boot field. Its session must reach this USB connection first. */
    if ((g_reported_boot != sec_boot_id() || g_reported_usb_epoch != usb_console_epoch()) &&
        !emit_self(1)) { return; }
    pu_emit_pps(g.pps_tick, g.utc_s, g.ppb, g.pps_valid, g.fix, g.sats, g.span_s, g.seg, g.n);
}

/* USB unplugged: no radio activity until VBUS returns, then reboot into a new session. */
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

/* Send the boot fault on ID, not at enumeration: taking it before a console opens loses it. */
static void master_hello(void)
{
    if (!usb_console_ready()) { return; }
    pu_emit_identity(node_devid_hi(), node_devid_lo(), 1, fault_reset_reason(), radio_config());
    uint32_t pc, lr, cfsr;
    const char *cause;
    if (fault_take_report(&pc, &lr, &cause, &cfsr)) { pu_emit_fault(pc, lr, cause, cfsr); }
}

/* Recheck for every command, including faults detected during a USB burst. */
static void master_command(pu_cmd_t cmd, int st, uint32_t now, uint8_t idle_seq)
{
    if (cmd == PU_CMD_NONE) { return; }
    int ready = st == 0 && master_clock_check() && sec_ready() && !radio_needs_reset();
    switch (cmd) {
    case PU_CMD_ID:
        master_hello();
        pu_emit_ack("ID");
        break;
    case PU_CMD_STATUS:
        pu_emit_heartbeat(ready ? capture_now64() : 0, now,
                          st == 0 ? (uint8_t)(g_mac.seq - 1u) : idle_seq,
                          st == 0 ? (int)reg_count(&g_mac.reg) : 0);
        if (st == 0) { mac_master_report(&g_mac, 0); }
        emit_self(ready);
        if (ready) { emit_pps(); }
        pu_emit_ack("STATUS");
        break;
    case PU_CMD_PING:
        pu_emit_ack("PING");
        break;
    case PU_CMD_SETKEY:
        if (provision_key()) { g_rekey = 1; }
        break;
    case PU_CMD_GETKEY:
        read_key();
        break;
    case PU_CMD_RADIO:
        provision_radio(1, st == 0);
        break;
    case PU_CMD_EVENT_ACK:
        (void)mq_ack(&g_queue, pu_ack_hseq(), pu_ack_boot(), pu_ack_crc());
        break;
    case PU_CMD_CLOCK:
        if (!ready) { pu_emit_err("clock"); break; }
        pu_emit_clock(pu_clock_token(), capture_now64(), sec_boot_id());
        break;
    case PU_CMD_CHECKPOINT:
        if (!ready) { pu_emit_err("clock"); break; } /* no beacons without a timebase */
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

static void run_master(int st)
{
    mq_init(&g_queue);
    g_hfxo_stops = board_hfxo_stops();
    if (st == 0) { mac_master_init(&g_mac, &g_queue); }

    mp_state_t power = MP_RUN;
    retry_t retry;
    retry_init(&retry, board_millis(), RADIO_BOOT_RETRY_MS);
    uint32_t last = board_millis();
    uint8_t idle_seq = 0;
    for (;;) {
        uint32_t now = board_millis();
        service(now);
        if (mp_step(&power, usb_vbus_present()) == MP_STOP) { master_stopped(&power, st == 0); }
        el_report(now);

        int c;
        while ((c = usb_read_byte()) >= 0) {
            master_command(pu_feed(c), st, now, idle_seq);
        }
        mq_pump(&g_queue, now);

        if (st != 0) {
            if (retry_bring_up(&retry, 1, now)) {
                st = 0;
                g_hfxo_stops = board_hfxo_stops();
                mac_master_init(&g_mac, &g_queue);
                continue;
            }
            if ((uint32_t)(now - last) >= 1000u) {
                last = now;
                pu_emit_heartbeat(0, now, idle_seq++, 0);
                if ((uint8_t)(idle_seq % CHECKPOINT_PERIOD_BEACONS) == 0u) { emit_self(0); }
            }
            continue;
        }
        if (!master_clock_check()) { continue; }
        gps_poll();
        if (sec_ready() && !radio_needs_reset()) { emit_pps(); }

        if (mac_master_step(&g_mac)) {
            uint8_t sent = (uint8_t)(g_mac.seq - 1u);
            /* Without a key sec_seal refuses: no beacon goes out; tell the PC. */
            if (!sec_provisioned() && (sent % 5u) == 0u) { pu_emit_err("noprov"); }
            int ready = sec_ready() && !radio_needs_reset();
            pu_emit_heartbeat(ready ? capture_now64() : 0, now, sent, (int)reg_count(&g_mac.reg));
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
    radio_cfg_t radio;
    radio_cfg_load(&radio);
    radio_set_config(&radio);
    usb_init();

    int master = role_decide_master();

    /* USB may have started HFXO during role resolution: check the clock source only now. */
    int st = bring_up(master);

    if (master) {
        run_master(st);
    } else {
        run_sensor(st);
    }
    return 0;
}
