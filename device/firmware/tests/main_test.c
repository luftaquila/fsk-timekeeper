#include <assert.h>
#include <stdio.h>
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
int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN]) { (void)key; return 0; }
void mac_master_session(mac_master_t *m) { (void)m; sessions++; }
void mac_master_checkpoint(mac_master_t *m) { (void)m; checkpoints++; }
void mac_master_report(mac_master_t *m, int changed) { (void)m; (void)changed; }
uint8_t fault_reset_reason(void) { return 0; }
uint32_t node_devid_hi(void) { return 1; }
uint32_t node_devid_lo(void) { return 2; }
int16_t meas_temp_c10(void) { return 250; }
uint16_t meas_vddh_mv(void) { return 4000; }
void gps_report(gps_report_t *r) { memset(r, 0, sizeof *r); }
int usb_write(const char *line) { assert(strlen(output) + strlen(line) < sizeof output); strcat(output, line); return 1; }

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
    assert(strstr(output, " M 13 922100 7 250.00 16000 0 -3 0\n") && strstr(output, "A R OK\n"));
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

int main(void)
{
    mq_init(&g_queue);
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
    return 0;
}
