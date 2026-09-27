/* Every USB line type, printed by the firmware's own formatters with extreme values.
 * `make test` compares the output with usb_lines.txt, which the console's
 * firmware-lines test parses: both sides read the same lines. */
#include <stdio.h>
#include <string.h>

#include "../src/master_queue.h"
#include "../src/proto_usb.h"

uint32_t board_millis(void) { return 0; }
int usb_write(const char *s) { return fputs(s, stdout) >= 0; }

/* E lines of every kind, formatted by the host queue. */
static void events(void)
{
    static mq_t q;
    mq_init(&q);
    uplink_pl_t u;
    memset(&u, 0, sizeof u);
    u.kind = UL_KIND_EDGES;
    u.flags = HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED;
    u.ev_seq = 65535;
    u.master_boot_id = 4294967295u;
    u.capture_seq = 4294967294u; /* wraps inside the line */
    u.tick = 18446744073709000000ull;
    u.sync_age_ms = 65535;
    u.u.edges.count = UL_EDGES_MAX;
    for (unsigned i = 0; i < UL_EDGES_MAX - 1; i++) { u.u.edges.dt[i] = 100000u + i; }
    mq_push_uplink(&q, 0xDEADBEEFu, 4294967295u, &u, -148.5f, -20.25f);

    u.kind = UL_KIND_LOSS;
    u.flags = EVENT_TIME_UNKNOWN;
    u.u.loss.end_seq = 3;
    u.u.loss.end_tick = 0;
    u.tick = 0;
    mq_push_uplink(&q, 0x0D2243B0u, 7u, &u, 0.0f, 0.0f);

    u.flags = HEALTH_EVENT_REQUIRED;
    u.capture_seq = 10;
    u.u.loss.end_seq = 12;
    u.tick = 5000;
    u.u.loss.end_tick = 9000;
    mq_push_uplink(&q, 0x0D2243B0u, 7u, &u, -60.0f, 7.5f);

    u.kind = UL_KIND_CHECKPOINT;
    u.capture_seq = 12;
    u.tick = 123456789;
    mq_push_uplink(&q, 0x0D2243B0u, 7u, &u, -60.0f, 7.5f);

    mq_push_timebase_end(&q, 11u, 5000u);
    for (unsigned i = 0; i < mq_depth(&q); i++) { fputs(q.item[i].line, stdout); }
}

static void diagnostics(void)
{
    pu_diag_t d;
    memset(&d, 0, sizeof d);
    d.node_id = 0x0D2243B0u;
    d.state = PU_STATE_STALE;
    d.skew_ppm = -32768;
    d.rx_miss = 65535;
    d.beacon_gap = 255;
    d.last_seen_ms = 4321;
    d.rssi = -91.5f;
    d.snr = 9.25f;
    d.lat_ms = 22;
    d.temp_c10 = -105;
    d.batt_mv = 3987;
    d.sec_drop = 2;
    d.provisioned = 1;
    d.health = HEALTH_SYNC_VALID | HEALTH_CLOCK_XTAL;
    d.sync_age_ms = 800;
    d.capture_overflow = 1;
    d.fifo_drop = 2;
    d.err_flags = ERR_RADIO_RESET | ERR_ACK;
    d.reset_reason = RESET_FAULT;
    d.sensor_boot_id = 305419896u;
    d.master_boot_id = 3735928559u;
    pu_emit_diag(&d);

    memset(&d, 0, sizeof d);
    d.is_master = 1;
    d.state = PU_STATE_OK;
    d.provisioned = 1;
    d.health = HEALTH_EVENT_REQUIRED;
    d.temp_c10 = 305;
    d.batt_mv = 4980;
    d.queue_depth = 3;
    d.queue_overflow = 1;
    d.ver_drop = 5;
    d.tx_drop = 9;
    d.master_boot_id = 3735928559u;
    pu_emit_diag(&d);
}

int main(void)
{
    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 1, RESET_PIN | RESET_FAULT);
    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 0, 0);
    pu_emit_heartbeat(UINT64_MAX, 12345, 200, 3);
    events();
    diagnostics();
    pu_emit_pps(123456789012345678ull, 1727500000u, -1234, 1, 1, 9, 64, 3, 17);
    pu_emit_clock("0123456789abcdef0123456789abcdef", 99, 7);
    pu_emit_ack("CP");
    pu_emit_err_count("radio_reset", 3);
    pu_emit_fault(0x00027F38u, 0x0002A001u, "hard", 0x00000400u);
    return 0;
}
