/* Host stand-in of the camera master for the camera host's tests (photofinish/app): the firmware's
 * camera.c, cam_track.c, proto_usb.c and master_queue.c over stdin/stdout. Once a session starts,
 * a simulated camera exposes one line every period ticks of a 16 MHz clock that follows the host's
 * real time; argv[1] (optional) drops that many triggers at line 2000, so the strobe count falls
 * behind the trigger schedule. E lines are acknowledged as soon as they are written (no console). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "../src/camera.h"
#include "nrf.h"

NRF_GPIO_Type test_gpio[2];
NRF_TIMER_Type test_timer4;
NRF_GPIOTE_Type test_gpiote;
NRF_PPI_Type test_ppi;

#define DELAY_TICKS 37u

static struct timespec t0;
static uint64_t start_tick;
static uint64_t session_rng = 1000;
static unsigned skip_lines;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t.tv_nsec - t0.tv_nsec);
}

uint64_t capture_now64(void) { return now_ns() * 16u / 1000u; }
uint32_t board_millis(void) { return (uint32_t)(now_ns() / 1000000u); }
int board_hfclk_xtal(void) { return 1; }
int entropy_boot_id(uint64_t *out) { *out = ++session_rng; return 1; }
uint32_t node_sender_id(void) { return 0x89EF4567u; }
int usb_console_ready(void) { return 1; }
uint32_t usb_console_epoch(void) { return 0; }
int usb_write(const char *s) { fputs(s, stdout); fflush(stdout); return 1; }

void capture_strobe_enable(void) {}
void capture_strobe_reset(void)
{
    start_tick = capture_now64();
    test_timer4.TASKS_START = test_timer4.TASKS_STOP = 0;
}

/* Triggers since the session start, and the strobes the camera answered them with. */
static uint64_t triggers(void)
{
    uint32_t period = test_timer4.CC[0];
    if (!test_timer4.TASKS_START || test_timer4.TASKS_STOP || !period) { return 0; }
    return (capture_now64() - start_tick) / period;
}
static uint64_t strobes_of(uint64_t n)
{
    return skip_lines && n >= 2000u ? (n - 2000u > skip_lines ? n - skip_lines : 2000u) : n;
}
/* The trigger strobe s answered: every one but the dropped ones. */
static uint64_t trigger_of(uint64_t s)
{
    return skip_lines && s > 2000u ? s + skip_lines : s;
}
uint32_t capture_strobe_count(void) { return (uint32_t)strobes_of(triggers()); }
int capture_strobe_sample(uint32_t *count, uint64_t *tick)
{
    uint64_t s = strobes_of(triggers());
    if (!s) { return 0; }
    *count = (uint32_t)s;
    *tick = start_tick + trigger_of(s) * test_timer4.CC[0] + DELAY_TICKS; /* the last strobe's */
    return 1;
}

int main(int argc, char **argv)
{
    static mq_t q;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    skip_lines = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
    mq_init(&q);
    camera_init();
    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 1, 0, &(radio_cfg_t)RADIO_CFG_DEFAULT);
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        struct timeval tv = { 0, 1000 };
        if (select(1, &fds, NULL, NULL, &tv) > 0) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) { return 0; }
            for (ssize_t i = 0; i < n; i++) {
                pu_cmd_t cmd = pu_feed(buf[i]);
                uint32_t now = board_millis();
                if (cmd == PU_CMD_CAMERA) {
                    camera_command(pu_camera(), &q, 77u, now);
                } else if (cmd == PU_CMD_STATUS) {
                    pu_diag_t d;
                    if (camera_diag(&d, now)) { d.master_boot_id = 77u; pu_emit_diag(&d); }
                    pu_emit_ack("STATUS");
                } else if (cmd == PU_CMD_ID) {
                    pu_emit_identity(0x0123ABCDu, 0x89EF4567u, 1, 0, &(radio_cfg_t)RADIO_CFG_DEFAULT);
                    pu_emit_ack("ID");
                } else if (cmd == PU_CMD_BAD) {
                    pu_emit_err("badcmd");
                }
            }
        }
        camera_poll(board_millis());
        while (mq_depth(&q)) { /* write and acknowledge at once */
            const mq_item_t *h = &q.item[q.head];
            usb_write(h->line);
            mq_ack(&q, h->hseq, h->master_boot_id, h->crc);
        }
    }
}
