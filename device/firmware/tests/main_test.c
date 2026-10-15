#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define main firmware_main
#include "../src/main.c"
#undef main

static int xtal = 1, boot_ready = 1, ready = 1, radio_fault;
static uint32_t stops, now;
static uint64_t boot = UINT64_C(9007199254740993);
static unsigned sessions, inits, checkpoints, capture_inits;
static char output[16000];
NRF_GPIO_Type test_gpio[2];
uint32_t board_millis(void) { return now; }
uint32_t board_hfxo_stops(void) { return stops; }
int board_hfclk_xtal(void) { return xtal; }
uint64_t capture_now64(void) { return 1234567; }
void capture_init(void) { capture_inits++; }
void capture_pps_enable(void) {}
void capture_sensor_enable(void) {}
void gps_init(void) {}
static int begin_fail, rx_fail, init_pending, configured;
static unsigned begin_calls;
int radio_begin(void) {
    begin_calls++; init_pending = 1; configured = !begin_fail;
    return begin_fail ? -1 : 0;
}
int radio_needs_reset(void) { return radio_fault || init_pending; }
static radio_cfg_t radio_cfg = RADIO_CFG_DEFAULT;
static unsigned cfg_writes, rx_starts;
static int cfg_fail;
void radio_set_config(const radio_cfg_t *c) { radio_cfg = *c; init_pending = 1; configured = 0; }
const radio_cfg_t *radio_config(void) { return &radio_cfg; }
int radio_start_rx(void) {
    rx_starts++;
    if (rx_fail) { return -1; }
    if (configured) { init_pending = 0; }
    return 0;
}
int radio_cfg_write(const radio_cfg_t *c) { (void)c; cfg_writes++; return cfg_fail ? -1 : 0; }
void sec_init(void) { boot++; inits++; boot_ready = ready = 0; }
uint64_t sec_boot_id(void) { return boot; }
int sec_boot_ready(void) { return boot_ready; }
int sec_ready(void) { return ready; }
int sec_provisioned(void) { return 1; }
void sec_reload(void) {}
static uint8_t stored_key[KEYSTORE_KEY_LEN];
static int has_key, key_fail;
int keystore_load(uint8_t out[KEYSTORE_KEY_LEN]) {
    if (!has_key) { return 0; }
    memcpy(out, stored_key, sizeof stored_key);
    return 1;
}
int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN]) {
    if (key_fail) { return -1; }
    memcpy(stored_key, key, sizeof stored_key); has_key = 1;
    return 0;
}
void mac_master_session(mac_master_t *m) { (void)m; sessions++; }
void mac_master_checkpoint(mac_master_t *m) { (void)m; checkpoints++; }
void mac_master_report(mac_master_t *m, int changed) { (void)m; (void)changed; }
uint8_t fault_reset_reason(void) { return 0; }
static int fault_pending;
int fault_take_report(uint32_t *pc, uint32_t *lr, const char **cause, uint32_t *cfsr)
{
    if (!fault_pending) { return 0; }
    fault_pending = 0;
    *pc = 123; *lr = 456; *cause = "hard"; *cfsr = 0;
    return 1;
}
uint32_t node_devid_hi(void) { return 1; }
uint32_t node_devid_lo(void) { return 2; }
int16_t meas_temp_c10(void) { return 250; }
uint16_t meas_vddh_mv(void) { return 4000; }
static int pps_pending;
int gps_report_due(void) { return pps_pending; }
void gps_report(gps_report_t *r) {
    assert(pps_pending);
    memset(r, 0, sizeof *r);
    r->pps_tick = 16000000;
    pps_pending = 0;
}
static int console_open = 1;
int usb_console_ready(void) { return console_open; }
uint32_t usb_console_epoch(void) { return 0; }
int usb_write(const char *line) { if (!console_open) { return 0; } assert(strlen(output) + strlen(line) < sizeof output); strcat(output, line); return 1; }

static void command(pu_cmd_t cmd)
{
    output[0] = 0;
    master_command(cmd, 0, now, 0);
}

static pu_cmd_t feed(const char *line)
{
    pu_cmd_t cmd = PU_CMD_NONE;
    for (const char *p = line; *p; p++) { cmd = pu_feed(*p); }
    return cmd;
}

/* R stores the settings, reconfigures the running radio and answers I, then A R. */
static void radio_settings(void)
{
    radio_fault = 0;
    assert(feed("R 922100 -3 0\n") == PU_CMD_RADIO);
    command(PU_CMD_RADIO);
    assert(cfg_writes == 1 && rx_starts == 1);
    assert(radio_cfg.freq_khz == 922100u && radio_cfg.power_dbm == -3 && radio_cfg.lbt == 0u);
    assert(strstr(output, " M 14 922100 7 250.00 16000 0 -3 0\n") && strstr(output, "A R OK\n"));
    cfg_fail = 1;
    assert(feed("R 930000 22 1\n") == PU_CMD_RADIO);
    command(PU_CMD_RADIO);
    assert(strstr(output, "X cfgfail") && !strstr(output, "A R") && radio_cfg.freq_khz == 922100u);
    cfg_fail = 0;
    uint64_t saved_boot = boot;
    unsigned saved_sessions = sessions, saved_inits = capture_inits;
    assert(mq_push_timebase_end(&g_queue, boot, capture_now64()));
    unsigned saved_depth = mq_depth(&g_queue);
    for (unsigned failure = 0; failure < 2; failure++) {
        begin_fail = failure == 0;
        rx_fail = failure == 1;
        unsigned before_rx = rx_starts;
        assert(feed("R 923100 12 1\n") == PU_CMD_RADIO);
        command(PU_CMD_RADIO);
        assert(strstr(output, "923100 7 250.00 16000 0 12 1\nX cfgapply\n"));
        assert(!strstr(output, "A R") && radio_needs_reset());
        assert(rx_starts == before_rx + (failure == 1));
        command(PU_CMD_STATUS); assert(strstr(output, "D 0 LOST"));
        command(PU_CMD_CLOCK); assert(strstr(output, "X clock"));
        unsigned before_cp = checkpoints;
        command(PU_CMD_CHECKPOINT); assert(strstr(output, "X clock") && checkpoints == before_cp);
        assert(boot == saved_boot && sessions == saved_sessions && capture_inits == saved_inits);
        assert(mq_depth(&g_queue) == saved_depth);
    }
    begin_fail = rx_fail = 0;
    assert(radio_begin() == 0 && radio_start_rx() == 0);
    command(PU_CMD_STATUS); assert(strstr(output, "D 0 OK"));
    unsigned before_begin = begin_calls;
    assert(feed("R 923300 12 1\n") == PU_CMD_RADIO);
    output[0] = 0;
    master_command(PU_CMD_RADIO, -1, now, 0);
    assert(strstr(output, "923300 7 250.00 16000 0 12 1\nX cfgpending\n"));
    assert(!strstr(output, "A R") && begin_calls == before_begin);
    assert(radio_cfg.freq_khz == 923300u);
    const char *bad[] = { "R 802999 0 1\n", "R 930001 0 1\n", "R 922900 23 1\n", "R 922900 -10 1\n",
                          "R 922900 0 2\n", "R 922900 0\n", "R 922900 +1 1\n", "R 922900 0 1 \n" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) { assert(feed(bad[i]) == PU_CMD_BAD); }
    puts("PASS main_radio_settings");
}

static void key_settings(void)
{
    const char *key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(feed("?KEY\n") == PU_CMD_GETKEY);
    command(PU_CMD_GETKEY);
    assert(strcmp(output, "K NONE\n") == 0);
    char line[68];
    snprintf(line, sizeof line, "K %s\n", key);
    assert(feed(line) == PU_CMD_SETKEY);
    command(PU_CMD_SETKEY);
    assert(strcmp(output, "A K OK\n") == 0);
    command(PU_CMD_GETKEY);
    assert(strlen(output) == 67 && strncmp(output + 2, key, 64) == 0);
    key_fail = 1;
    assert(feed("K ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n") == PU_CMD_SETKEY);
    command(PU_CMD_SETKEY);
    assert(strcmp(output, "X keyfail\n") == 0);
    command(PU_CMD_GETKEY);
    assert(strncmp(output + 2, key, 64) == 0);
    /* Key readback must work even while radio bring-up is pending. */
    output[0] = 0;
    master_command(PU_CMD_GETKEY, -1, now, 0);
    assert(strncmp(output + 2, key, 64) == 0);
    assert(feed("?KEY extra\n") == PU_CMD_BAD);
    puts("PASS main_key_readback");
}

static void assert_queue_report(unsigned depth, unsigned overflow, uint16_t other_flags)
{
    output[0] = 0;
    emit_self(0);
    assert(strncmp(output, "D 0 ", 4) == 0);
    char copy[sizeof output];
    strcpy(copy, output);
    unsigned field = 0;
    for (char *p = strtok(copy, " \n"); p; p = strtok(NULL, " \n"), field++) {
        if (field == 20) { assert(strtoul(p, NULL, 10) == depth); }
        if (field == 21) { assert(strtoul(p, NULL, 10) == overflow); }
        if (field == 22) {
            uint16_t flags = (uint16_t)strtoul(p, NULL, 10);
            assert((flags & ERR_QUEUE_FULL) == (depth == MASTER_EVENT_QUEUE_LEN ? ERR_QUEUE_FULL : 0));
            assert((flags & (uint16_t)~ERR_QUEUE_FULL) == other_flags);
        }
    }
    assert(field > 22);
}

static void queue_pressure_report(void)
{
    mq_init(&g_queue);
    console_open = 0;
    /* No console ACK: the full queue keeps every accepted record. */
    for (unsigned i = 0; i < MASTER_EVENT_QUEUE_LEN; i++) {
        assert(mq_push_timebase_end(&g_queue, boot, i));
    }
    console_open = 1;
    assert_queue_report(MASTER_EVENT_QUEUE_LEN, 0, (uint16_t)(el_flags() & (uint16_t)~ERR_QUEUE_FULL));
    console_open = 0;
    uint32_t previous = el_count(EL_QUEUE_FULL);
    for (unsigned i = 0; i < 751; i++) {
        assert(!mq_push_timebase_end(&g_queue, boot, 99));
    }
    assert(g_queue.overflow == 751 && el_count(EL_QUEUE_FULL) == previous + 751);
    uint16_t other_flags = (uint16_t)(el_flags() & (uint16_t)~ERR_QUEUE_FULL);
    output[0] = 0;
    mq_pump(&g_queue, now);
    assert(!output[0] && mq_depth(&g_queue) == MASTER_EVENT_QUEUE_LEN);

    console_open = 1;
    assert_queue_report(MASTER_EVENT_QUEUE_LEN, 751, other_flags);
    unsigned acknowledged = 0;
    while (mq_depth(&g_queue)) {
        const mq_item_t *head = &g_queue.item[g_queue.head];
        assert(mq_ack(&g_queue, head->hseq, head->master_boot_id, head->crc));
        acknowledged++;
        assert_queue_report(MASTER_EVENT_QUEUE_LEN - acknowledged, 751, other_flags);
    }
    assert(acknowledged == MASTER_EVENT_QUEUE_LEN);
    assert(el_count(EL_QUEUE_FULL) == previous + 751);
    output[0] = 0;
    el_report(now);
    assert(strstr(output, "X queue_full "));
    puts("PASS main_queue_pressure_recovers_without_losing_history");
}

int main(void)
{
    mq_init(&g_queue);
    command(PU_CMD_STATUS);
    assert(!strstr(output, "P ") && strstr(output, "A STATUS OK"));
    pps_pending = 1;
    command(PU_CMD_STATUS);
    assert(strstr(output, "P 16000000 ") && !pps_pending);
    command(PU_CMD_STATUS);
    assert(!strstr(output, "P "));
    puts("PASS main_pps_status_does_not_repeat");
    command(PU_CMD_CLOCK);
    assert(strstr(output, "9007199254740993"));
    xtal = 0; stops++;
    command(PU_CMD_CLOCK); assert(strstr(output, "X clock"));
    assert(mq_depth(&g_queue) == 1 && g_queue.item[0].master_boot_id == boot);
    command(PU_CMD_CHECKPOINT); assert(checkpoints == 0 && strstr(output, "X clock"));
    command(PU_CMD_STATUS); assert(strstr(output, "D 0 LOST") && !strstr(output, "P "));
    assert(mq_depth(&g_queue) == 1);
    xtal = 1;
    command(PU_CMD_CLOCK); assert(inits == 1 && sessions == 1 && strstr(output, "X clock"));
    command(PU_CMD_CLOCK); assert(inits == 1 && strstr(output, "X clock"));
    boot_ready = ready = 1;
    command(PU_CMD_CLOCK); assert(strstr(output, "9007199254740994"));
    command(PU_CMD_CHECKPOINT); assert(checkpoints == 1);
    const mq_item_t *old = &g_queue.item[g_queue.head];
    assert(mq_ack(&g_queue, old->hseq, old->master_boot_id, old->crc));
    assert(capture_inits == 0); /* clock-session recovery never resets TIMER1 */
    radio_fault = 1;
    command(PU_CMD_CLOCK); assert(strstr(output, "X clock"));
    command(PU_CMD_STATUS); assert(strstr(output, "D 0 LOST"));
    puts("PASS main_clock_commands_recovery");
    radio_settings();
    key_settings();
    console_open = 0;
    fault_pending = 1;
    output[0] = 0;
    master_hello();
    assert(fault_pending && !output[0]);
    console_open = 1;
    command(PU_CMD_ID);
    assert(!fault_pending && strstr(output, "X fault ") && strstr(output, "A ID OK"));
    command(PU_CMD_ID);
    assert(!strstr(output, "X fault "));
    puts("PASS main_deferred_boot_fault");
    queue_pressure_report();
    return 0;
}
