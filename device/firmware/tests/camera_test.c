/* Camera line timing (cam_track.c) and the V command flow (camera.c) against a simulated camera:
 * trigger k exposes at start + k * period + DELAY unless the camera skips it. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cam_track.h"
#include "../src/camera.h"
#include "nrf.h"

NRF_GPIO_Type test_gpio[2];
NRF_TIMER_Type test_timer4;
NRF_GPIOTE_Type test_gpiote;
NRF_PPI_Type test_ppi;

#define DELAY 37u
#define BOOT  77u

static uint64_t now_tick = 5000000, last_strobe, t_start, triggers, skip_from, skip_to;
static uint32_t now_ms = 1000, strobes, period;
static uint64_t session_rng = 0x1234;
static char output[4096];
static mq_t q;

uint32_t board_millis(void) { return now_ms; }
int board_hfclk_xtal(void) { return 1; }
uint64_t capture_now64(void) { return now_tick; }
void capture_strobe_enable(void) {}
void capture_strobe_reset(void) { strobes = 0; }
uint32_t capture_strobe_count(void) { return strobes; }
int capture_strobe_sample(uint32_t *count, uint64_t *tick)
{
    if (!strobes) { return 0; }
    *count = strobes;
    *tick = last_strobe;
    return 1;
}
int entropy_boot_id(uint64_t *out) { *out = ++session_rng; return 1; }
uint32_t node_sender_id(void) { return 0x89EF4567u; }
int usb_console_ready(void) { return 1; }
uint32_t usb_console_epoch(void) { return 0; }
int usb_write(const char *s)
{
    assert(strlen(output) + strlen(s) < sizeof output);
    strcat(output, s);
    return 1;
}

/* n more triggers; those numbered skip_from..skip_to expose nothing. */
static void fire(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        triggers++;
        now_tick = t_start + triggers * period + DELAY + 1u;
        if (triggers >= skip_from && triggers <= skip_to) { continue; }
        strobes++;
        last_strobe = t_start + triggers * period + DELAY;
    }
}

/* ms of triggers, one anchor every CAM_SAMPLE_MS. */
static void advance(unsigned ms)
{
    for (unsigned at = 0; at < ms; at += CAM_SAMPLE_MS) {
        fire(CAM_SAMPLE_MS * TICKS_PER_MS / period);
        now_ms += CAM_SAMPLE_MS;
        camera_poll(now_ms);
    }
}

static uint64_t exposure(uint64_t trigger) { return t_start + trigger * period + DELAY; }

static const char *send(const char *line)
{
    output[0] = 0;
    pu_cmd_t cmd = PU_CMD_NONE;
    for (const char *p = line; *p; p++) { cmd = pu_feed(*p); }
    assert(cmd == PU_CMD_CAMERA);
    camera_command(pu_camera(), &q, BOOT, now_ms);
    return output;
}

static const char *tail(void)
{
    assert(mq_depth(&q));
    return q.item[(q.head + q.count - 1u) % MASTER_EVENT_QUEUE_LEN].line;
}

/* Token i of a line as a number (tokens 2 and 3 are text). */
static uint64_t token(const char *line, unsigned i)
{
    const char *p = line;
    for (unsigned k = 0; k < i; k++) { p = strchr(p, ' '); assert(p); p++; }
    return strtoull(p, NULL, 10);
}

static char kind(const char *line)
{
    const char *p = strchr(strchr(line + 2, ' ') + 1, ' ');
    return p[1];
}

static void drain(void)
{
    while (mq_depth(&q)) {
        const mq_item_t *h = &q.item[q.head];
        assert(mq_ack(&q, h->hseq, h->master_boot_id, h->crc));
    }
}

static void track_grid(void)
{
    cam_track_t c;
    uint64_t tick, lo, hi, after, upto;
    cam_track_init(&c, 400, 1000);
    assert(cam_track_line(&c, 1, &tick, &lo, &hi) == CAM_LINE_WAIT);
    assert(cam_track_floor(&c, 5) == 1000 && cam_track_known(&c) == 0);
    cam_track_anchor(&c, 100, 1000 + 100 * 400 + 37);
    cam_track_anchor(&c, 600, 1000 + 600 * 400 + 37 + CAM_JITTER_TICKS); /* jitter at the bound */
    assert(!c.broken && cam_track_known(&c) == 600);
    assert(cam_track_line(&c, 300, &tick, &lo, &hi) == CAM_LINE_EXACT);
    assert(tick == 1000 + 300 * 400 + 37 && lo == 100 && hi == 600);
    assert(cam_track_line(&c, 100, &tick, &lo, &hi) == CAM_LINE_BOUND); /* line 99 precedes the first anchor */
    assert(cam_track_line(&c, 601, &tick, &lo, &hi) == CAM_LINE_WAIT);
    assert(cam_track_range(&c, 50, 60, &after, &upto) && after == 1000 && upto == 1000 + 100 * 400 + 37);
    assert(!cam_track_range(&c, 500, 601, &after, &upto));
    assert(cam_track_floor(&c, 5000) == 1000 + 600 * 400 + 37 + CAM_JITTER_TICKS);
    cam_track_anchor(&c, 500, 1); /* not above the last anchor: ignored */
    assert(!c.broken && cam_track_known(&c) == 600);

    /* Late (a skipped trigger, or a strobe the master missed) or early (an extra exposure, or a
     * counted glitch): the master cannot tell which, so line numbers are no longer trusted. */
    cam_track_t late = c, early = c;
    cam_track_anchor(&late, 1100, 1000 + 1101 * 400 + 37);
    assert(late.broken && cam_track_known(&late) == 600);
    cam_track_anchor(&early, 1100, 1000 + 1099 * 400 + 37);
    assert(early.broken && cam_track_known(&early) == 600);
    cam_track_anchor(&late, 1200, 1000 + 1200 * 400 + 37); /* back on the grid: still broken */
    assert(late.broken && cam_track_known(&late) == 600);
    puts("PASS camera_track_grid");
}

static void parse(void)
{
    const char *bad[] = { "V S 275\n", "V S 16001\n", "V S\n", "V S 400 \n", "V C 0 5 1 1\n",
                          "V C 1 0 1 1\n", "V C 1 5 1000 1\n", "V C 1 5 1 101\n", "V C 1 5 1\n",
                          "V L 1 9 8\n", "V L 1 9\n", "V K 1\n", "V X 1 2\n", "V \n",
                          "V K 1 18446744073709551616\n", "V K 4294967296 5\n" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pu_cmd_t cmd = PU_CMD_NONE;
        for (const char *p = bad[i]; *p; p++) { cmd = pu_feed(*p); }
        assert(cmd == PU_CMD_BAD);
    }
    pu_cmd_t cmd = PU_CMD_NONE;
    for (const char *p = "V L 7 10 10\n"; *p; p++) { cmd = pu_feed(*p); }
    assert(cmd == PU_CMD_CAMERA && pu_camera()->kind == 'L' && pu_camera()->q == 7);
    assert(pu_camera()->line == 10 && pu_camera()->last == 10);
    for (const char *p = "V S 0\n"; *p; p++) { cmd = pu_feed(*p); }
    assert(cmd == PU_CMD_CAMERA && pu_camera()->kind == 'S' && pu_camera()->period == 0);
    puts("PASS camera_parse");
}

static void hardware(void)
{
    camera_init();
    assert(test_timer4.SHORTS == TIMER_SHORTS_COMPARE0_CLEAR_Msk && test_timer4.PRESCALER == 0);
    assert(test_gpiote.CONFIG[3] == (GPIOTE_CONFIG_MODE_Task | (31u << GPIOTE_CONFIG_PSEL_Pos)));
    assert(test_ppi.CH[3].EEP == (uint32_t)&test_timer4.EVENTS_COMPARE[0]);
    assert(test_ppi.CH[3].TEP == (uint32_t)&test_gpiote.TASKS_SET[3]);
    assert(test_ppi.CH[4].EEP == (uint32_t)&test_timer4.EVENTS_COMPARE[1]);
    assert(test_ppi.CH[4].TEP == (uint32_t)&test_gpiote.TASKS_CLR[3]);
    puts("PASS camera_hardware");
}

static void session(void)
{
    pu_diag_t d;
    mq_init(&q);
    assert(strstr(send("V C 1 5 7 90\n"), "X camoff") && !camera_diag(&d, now_ms));

    period = 400; t_start = now_tick; triggers = 0;
    memset(&test_timer4, 0, sizeof test_timer4);
    assert(!strcmp(send("V S 400\n"), "A V OK\n"));
    assert(test_timer4.CC[0] == 400 && test_timer4.CC[1] == 200 && test_timer4.TASKS_START);
    assert(camera_diag(&d, now_ms) && d.state == PU_STATE_OK && d.sensor_boot_id == session_rng);
    uint64_t session = d.sensor_boot_id;
    advance(100); /* 4000 lines, anchors every 400 */

    /* A crossing inside a run: exact tick, run bounds, one capture_seq. */
    assert(!strcmp(send("V C 1 2000 7 90\n"), "A V OK\n"));
    const char *e = tail();
    assert(!strncmp(e, "E 1 89EF4567 V 1 7 77 ", 22) && token(e, 7) == session);
    assert(token(e, 11) == 1 && token(e, 12) == 2000 && token(e, 13) == exposure(2000));
    assert(token(e, 14) == 400 && token(e, 15) < 2000 && token(e, 16) >= 2000);
    assert(token(e, 17) == 7 && token(e, 18) == 90);
    uint64_t last = exposure(2000);

    /* Resends and order. */
    unsigned depth = mq_depth(&q);
    assert(!strcmp(send("V C 1 2000 7 90\n"), "A V OK\n") && mq_depth(&q) == depth);
    assert(strstr(send("V C 3 2100 7 90\n"), "X camseq"));
    assert(strstr(send("V K 2 1999\n"), "X camseq"));
    assert(strstr(send("V C 2 2000 7 90\n"), "X camseq") && mq_depth(&q) == depth);

    /* A checkpoint at that line: through its exposure, no capture_seq. */
    assert(!strcmp(send("V K 2 2000\n"), "A V OK\n"));
    e = tail();
    assert(kind(e) == 'K' && token(e, 11) == 1 && token(e, 12) == exposure(2000));

    /* Past the last anchor: wait for the next one. */
    fire(5);
    assert(strstr(send("V C 3 4003 0 0\n"), "X camwait"));
    advance(10);
    assert(!strcmp(send("V C 3 4003 0 0\n"), "A V OK\n"));
    e = tail();
    assert(kind(e) == 'V' && token(e, 11) == 2 && token(e, 13) == exposure(4003) && token(e, 17) == 0);
    assert(token(e, 13) > last);
    last = token(e, 13);

    /* Lost lines: (exposure of first - 1, exposure of last]. */
    assert(!strcmp(send("V L 4 4100 4200\n"), "A V OK\n"));
    e = tail();
    assert(kind(e) == 'L' && token(e, 11) == 3 && token(e, 12) == 3);
    assert(token(e, 13) == exposure(4099) + 1u && token(e, 14) == exposure(4200));
    last = token(e, 14);
    drain();

    /* Host queue full: resend later. */
    for (unsigned i = mq_depth(&q); i < MASTER_EVENT_QUEUE_LEN; i++) { assert(mq_push_timebase_end(&q, BOOT, i)); }
    assert(strstr(send("V K 5 4300\n"), "X camfull"));
    drain();
    assert(!strcmp(send("V K 5 4300\n"), "A V OK\n"));
    e = tail();
    assert(kind(e) == 'K' && token(e, 12) >= last); /* never behind an earlier record */
    assert(camera_diag(&d, now_ms) && d.fifo_drop == 1 && d.capture_overflow == 0);

    /* Link state follows the host's records. */
    assert(camera_diag(&d, now_ms + LINK_OK_MS + 1u) && d.state == PU_STATE_STALE);
    assert(camera_diag(&d, now_ms + LINK_STALE_MS + 1u) && d.state == PU_STATE_LOST);

    /* The camera skips three triggers: the strobe count falls behind the trigger schedule, as it
     * would if the master missed strobes. Line numbers can no longer be trusted: the session ends. */
    skip_from = triggers + 100u; skip_to = skip_from + 2u;
    advance(20);
    test_timer4.TASKS_STOP = 0;
    assert(strstr(send("V K 6 4300\n"), "X camline") && test_timer4.TASKS_STOP);
    assert(strstr(send("V K 6 4300\n"), "X camoff"));
    assert(camera_diag(&d, now_ms) && d.state == PU_STATE_LOST && !(d.health & HEALTH_SYNC_VALID));

    /* A new session restarts numbering, sequence and records. */
    skip_from = skip_to = 0; t_start = now_tick; triggers = 0;
    assert(!strcmp(send("V S 320\n"), "A V OK\n"));
    period = 320;
    assert(camera_diag(&d, now_ms) && d.sensor_boot_id != session && d.capture_overflow == 0);
    advance(50);
    drain();
    assert(!strcmp(send("V C 1 1000 3 70\n"), "A V OK\n"));
    e = tail();
    assert(token(e, 11) == 1 && token(e, 13) == exposure(1000) && token(e, 14) == 320);

    /* A line the strobes never reached: the numbering is off, the session ends. */
    char line[64];
    snprintf(line, sizeof line, "V C 2 %llu 1 1\n", (unsigned long long)strobes + 1u);
    assert(strstr(send(line), "X camline"));
    assert(!strcmp(send("V S 0\n"), "A V OK\n") && strstr(send("V K 2 100\n"), "X camoff"));
    puts("PASS camera_session");
}

int main(void)
{
    track_grid();
    parse();
    hardware();
    session();
    return 0;
}
