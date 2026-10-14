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
int radio_begin(void) { return radio_fault ? -1 : 0; }
int radio_needs_reset(void) { return radio_fault; }
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
    return 0;
}
