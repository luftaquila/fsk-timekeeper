/* Fault-reboot bookkeeping (fault.c, built with FAULT_HOST_TEST), the radio
 * hang check (hang_guard.h) and the master USB power policy (power.c). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/config.h"
#include "../src/fault.h"
#include "../src/hang_guard.h"
#include "../src/power.h"
#include "../src/proto_usb.h"

#define RR_RESETPIN (1u << 0)
#define RR_SREQ     (1u << 2)
#define RR_LOCKUP   (1u << 3)
#define RR_VBUS     (1u << 20)
#define RR_NFC      (1u << 19)

static void fault_reboots(void)
{
    fault_rec_t r;
    memset(&r, 0xA5, sizeof(r)); /* power-on: garbage */
    int report;
    assert(fault_eval_boot(&r, 0, &report) == 0 && !report && r.count == 0);

    /* three fault reboots are allowed, the fourth fault halts */
    for (unsigned i = 1; i <= FAULT_REBOOT_MAX; i++) {
        assert(fault_note(&r, 0x1000u + i, 0x2000u, 0x400u, FAULT_CAUSE_HARD) == 0);
        uint8_t reason = fault_eval_boot(&r, RR_SREQ, &report);
        assert(report && reason == (RESET_SREQ | RESET_FAULT) && r.count == i && !r.pending);
        assert(r.pc == 0x1000u + i && r.lr == 0x2000u && r.cause == FAULT_CAUSE_HARD);
    }
    assert(fault_note(&r, 1, 2, 3, FAULT_CAUSE_BUS) == 1);

    /* the user presses RST: the fault is still reported, the count restarts */
    uint8_t reason = fault_eval_boot(&r, RR_RESETPIN, &report);
    assert(report && reason == (RESET_PIN | RESET_FAULT) && r.count == 0);

    /* an ordinary soft reset (DFU touch, VBUS return) neither reports nor clears */
    assert(fault_note(&r, 1, 2, 3, FAULT_CAUSE_USAGE) == 0);
    (void)fault_eval_boot(&r, RR_SREQ, &report);
    assert(report && r.count == 1);
    assert(fault_eval_boot(&r, RR_SREQ, &report) == RESET_SREQ && !report && r.count == 1);

    /* a lockup (fault while handling a fault) counts like a fault reboot */
    (void)fault_eval_boot(&r, RR_LOCKUP, &report);
    assert(!report && r.count == 2);

    /* other bits map into the compact byte */
    assert(fault_eval_boot(&r, RR_VBUS | RR_NFC, &report) == (RESET_VBUS | RESET_OTHER) && r.count == 0);

    /* a corrupted record is rebuilt, not trusted */
    r.count = 99;
    assert(fault_eval_boot(&r, RR_SREQ, &report) == RESET_SREQ && !report && r.count == 0);
    printf("PASS fault_reboots\n");
}

static void hang_check(void)
{
    hang_guard_t g;
    hang_guard_reset(&g);
    /* BUSY waits that end: BUSY drops, an SPI transfer or a new radio call */
    assert(!hang_guard_poll(&g, 1, 100, RADIO_HANG_MS));
    assert(!hang_guard_poll(&g, 1, 100 + RADIO_HANG_MS - 1u, RADIO_HANG_MS));
    assert(!hang_guard_poll(&g, 0, 100 + RADIO_HANG_MS, RADIO_HANG_MS));
    assert(!hang_guard_poll(&g, 1, 5000, RADIO_HANG_MS));
    hang_guard_reset(&g);
    assert(!hang_guard_poll(&g, 1, 5000 + RADIO_HANG_MS, RADIO_HANG_MS)); /* times from here */
    /* BUSY high through a whole limit: the radio hung */
    assert(!hang_guard_poll(&g, 1, 6000 + RADIO_HANG_MS - 1u, RADIO_HANG_MS));
    assert(hang_guard_poll(&g, 1, 6000 + RADIO_HANG_MS, RADIO_HANG_MS));
    /* across the millis wrap */
    hang_guard_reset(&g);
    assert(!hang_guard_poll(&g, 1, UINT32_MAX - 9u, RADIO_HANG_MS));
    assert(!hang_guard_poll(&g, 1, RADIO_HANG_MS - 11u, RADIO_HANG_MS));
    assert(hang_guard_poll(&g, 1, RADIO_HANG_MS - 10u, RADIO_HANG_MS));
    /* every bounded wait is far shorter than the limit */
    assert(RADIO_SPI_TIMEOUT_MS * 4u < RADIO_HANG_MS && RADIO_PROBE_MS + RADIO_SPI_TIMEOUT_MS < RADIO_HANG_MS);
    printf("PASS hang_check\n");
}

static void power_policy(void)
{
    mp_state_t s = MP_RUN;
    assert(mp_step(&s, 1) == MP_NONE && s == MP_RUN);   /* suspended host keeps VBUS: no stop */
    assert(mp_step(&s, 0) == MP_STOP && s == MP_STOPPED);
    assert(mp_step(&s, 0) == MP_NONE);
    assert(mp_step(&s, 1) == MP_RESET);                  /* replugged: reboot into a new session */
    printf("PASS power_policy\n");
}

int main(void)
{
    fault_reboots();
    hang_check();
    power_policy();
    return 0;
}
