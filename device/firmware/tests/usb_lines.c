/* Every USB line type but J and the K reply, from the firmware's formatters with extreme values;
 * `make test` diffs it with usb_lines.txt, which the console's firmware-lines test parses. */
#include <stdio.h>
#include <string.h>

#include "../src/master_queue.h"
#include "../src/proto_usb.h"

uint32_t board_millis(void) { return 0; }
int usb_console_ready(void) { return 1; }
uint32_t usb_console_epoch(void) { return 0; }
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
    u.master_boot_id = UINT64_MAX;
    u.capture_seq = 4294967294u; /* wraps inside the line */
    u.tick = 18446744073709000000ull;
    u.sync_age_ms = 65535;
    u.u.edges.count = UL_EDGES_MAX;
    for (unsigned i = 0; i < UL_EDGES_MAX - 1; i++) { u.u.edges.dt[i] = 100000u + i; }
    mq_push_uplink(&q, 0xDEADBEEFu, UINT64_MAX, &u, -148.5f, -20.25f);

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
    for (unsigned i = 0; i < RADIO_HISTORY_FAULTS; i++) { u.u.history.faults[i] = UINT16_MAX; }
    u.u.history.recovery = (radio_recovery_diag_t){ 65535, 123, RADIO_RECOVERY_BEACON_LOSS | (RADIO_RECOVERY_OK << 4) };
    u.capture_seq = 12;
    u.tick = 123456789;
    mq_push_uplink(&q, 0x0D2243B0u, 7u, &u, -60.0f, 7.5f);

    mq_push_timebase_end(&q, 11u, 5000u);
    for (unsigned i = 0; i < mq_depth(&q); i++) { fputs(q.item[i].line, stdout); }
}

/* Camera node (the master's sender id, devid low word of the I lines) records. */
static void camera(void)
{
    static mq_t q;
    mq_init(&q);
    q.next_hseq = 6;
    pu_camera_crossing_t c = { UINT64_MAX, UINT64_MAX, 65535, HEALTH_EVENT_REQUIRED, 4294967295u,
                               18446744073709551614ull, 18446744073709000000ull, CAM_PERIOD_MAX,
                               18446744073709551000ull, UINT64_MAX, 999, 100 };
    mq_push_camera_crossing(&q, 0x89EF4567u, &c);
    pu_camera_crossing_t low = { 11, 42, 3, HEALTH_EVENT_REQUIRED, 1, 1201, 16480400, CAM_PERIOD_MIN,
                                 640, 1500, 0, 0 };
    mq_push_camera_crossing(&q, 0x89EF4567u, &low);

    uplink_pl_t u;
    memset(&u, 0, sizeof u);
    u.kind = UL_KIND_LOSS;
    u.flags = HEALTH_EVENT_REQUIRED;
    u.ev_seq = 4;
    u.master_boot_id = 11;
    u.capture_seq = u.u.loss.end_seq = 2;
    u.tick = 16500001;
    u.u.loss.end_tick = 16600000;
    mq_push_uplink(&q, 0x89EF4567u, 42, &u, 0.0f, 0.0f);
    u.kind = UL_KIND_CHECKPOINT;
    memset(&u.u, 0, sizeof u.u); /* the camera's checkpoints carry no radio history */
    u.ev_seq = 5;
    u.tick = 16700000;
    mq_push_uplink(&q, 0x89EF4567u, 42, &u, 0.0f, 0.0f);
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
    d.diag_valid = 1;
    d.health = HEALTH_SYNC_VALID | HEALTH_CLOCK_XTAL;
    d.sync_age_ms = 800;
    d.capture_overflow = 1;
    d.fifo_drop = 2;
    d.err_flags = ERR_ACK;
    d.reset_reason = RESET_FAULT;
    d.sensor_boot_id = UINT64_C(9007199254740993);
    d.master_boot_id = UINT64_MAX;
    pu_emit_diag(&d);

    memset(&d, 0, sizeof d);
    d.is_master = 1;
    d.diag_valid = 1;
    d.state = PU_STATE_OK;
    d.provisioned = 1;
    d.health = HEALTH_EVENT_REQUIRED;
    d.temp_c10 = 305;
    d.batt_mv = 4980;
    d.queue_depth = 3;
    d.queue_overflow = 1;
    d.ver_drop = 5;
    d.tx_drop = 9;
    d.master_boot_id = UINT64_MAX;
    pu_emit_diag(&d);

    memset(&d, 0, sizeof d); /* camera node */
    d.node_id = 0x89EF4567u;
    d.state = PU_STATE_OK;
    d.last_seen_ms = 830;
    d.lat_ms = 412;
    d.provisioned = 1;
    d.diag_valid = 1;
    d.health = HEALTH_EVENT_REQUIRED;
    d.capture_overflow = 2;
    d.fifo_drop = 1;
    d.sensor_boot_id = 42;
    d.master_boot_id = 11;
    pu_emit_diag(&d);
}

int main(void)
{
    const radio_cfg_t radio = RADIO_CFG_DEFAULT;
    const radio_cfg_t low = { RADIO_FREQ_MIN_KHZ, RADIO_POWER_MIN_DBM, 0 };
    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 1, RESET_PIN | RESET_FAULT, &radio);
    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 0, 0, &low);
    pu_emit_heartbeat(UINT64_MAX, 12345, 200, 3);
    events();
    camera();
    diagnostics();
    pu_emit_pps(123456789012345678ull, 1727500000u, -1234, 1, 1, 9, 64, 3, 17);
    pu_emit_clock("0123456789abcdef0123456789abcdef", 99, UINT64_MAX);
    pu_emit_ack("CP");
    pu_emit_ack("V");
    pu_emit_err("camwait");
    pu_emit_err_count("radio_reset", 3);
    pu_emit_fault(0x00027F38u, 0x0002A001u, "hard", 0x00000400u);
    return 0;
}
