#include "camera.h"

#include <string.h>

#include "board.h"
#include "cam_track.h"
#include "capture.h"
#include "config.h"
#include "entropy.h"
#include "gpio.h"
#include "node_id.h"
#include "nrf.h"

/* capture.c owns GPIOTE and PPI channels 0..2. */
#define CAM_GPIOTE_TRIG 3
#define CAM_PPI_SET     3
#define CAM_PPI_CLR     4
#define TRIG_PIN (PIN_CAMERA_TRIG % 32u)
#define TRIG_PRT (PIN_CAMERA_TRIG / 32u)

typedef struct {
    int      running;
    uint64_t session;    /* 0 before the first session */
    uint32_t q;          /* last host record taken */
    uint32_t seq;        /* capture_seq of the last V or L record */
    uint64_t line;       /* last line a record covered */
    uint64_t tick;       /* latest tick a record reached; records never go back */
    uint64_t lag_tick;   /* exposure tick of the last checkpoint's line */
    uint64_t count_hi;   /* strobe count above 32 bits */
    uint32_t count_lo;
    uint32_t sampled_ms, heard_ms;
    uint16_t lost;       /* L records, saturating */
    cam_track_t track;
} camera_t;

static camera_t s_cam;

/* 64-bit strobe count from a 32-bit read taken after the previous one. */
static uint64_t strobes(uint32_t low)
{
    if (low < s_cam.count_lo) { s_cam.count_hi += (uint64_t)1 << 32; }
    s_cam.count_lo = low;
    return s_cam.count_hi + low;
}

void camera_init(void)
{
    NRF_TIMER4->TASKS_STOP = 1;
    NRF_TIMER4->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER4->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER4->PRESCALER = 0; /* 16 MHz, the TIMER1 clock */
    NRF_TIMER4->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
    NRF_TIMER4->TASKS_CLEAR = 1;
    gpio_clear(PIN_CAMERA_TRIG);
    gpio_cfg_output(PIN_CAMERA_TRIG);
    NRF_GPIOTE->CONFIG[CAM_GPIOTE_TRIG] = ((uint32_t)GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) |
        (TRIG_PIN << GPIOTE_CONFIG_PSEL_Pos) | (TRIG_PRT << GPIOTE_CONFIG_PORT_Pos) |
        ((uint32_t)GPIOTE_CONFIG_POLARITY_None << GPIOTE_CONFIG_POLARITY_Pos) |
        ((uint32_t)GPIOTE_CONFIG_OUTINIT_Low << GPIOTE_CONFIG_OUTINIT_Pos);
    /* Rising edge at every whole period, falling edge half a period later. */
    NRF_PPI->CH[CAM_PPI_SET].EEP = (uint32_t)&NRF_TIMER4->EVENTS_COMPARE[0];
    NRF_PPI->CH[CAM_PPI_SET].TEP = (uint32_t)&NRF_GPIOTE->TASKS_SET[CAM_GPIOTE_TRIG];
    NRF_PPI->CH[CAM_PPI_CLR].EEP = (uint32_t)&NRF_TIMER4->EVENTS_COMPARE[1];
    NRF_PPI->CH[CAM_PPI_CLR].TEP = (uint32_t)&NRF_GPIOTE->TASKS_CLR[CAM_GPIOTE_TRIG];
    NRF_PPI->CHENSET = (1UL << CAM_PPI_SET) | (1UL << CAM_PPI_CLR);
    capture_strobe_enable();
}

void camera_stop(void)
{
    NRF_TIMER4->TASKS_STOP = 1;
    NRF_TIMER4->TASKS_CLEAR = 1;
    NRF_GPIOTE->TASKS_CLR[CAM_GPIOTE_TRIG] = 1;
    s_cam.running = 0;
}

static void start(uint64_t session, uint32_t period, uint32_t now_ms)
{
    capture_strobe_reset();
    memset(&s_cam, 0, sizeof s_cam);
    s_cam.running = 1;
    s_cam.session = session;
    s_cam.sampled_ms = s_cam.heard_ms = now_ms;
    cam_track_init(&s_cam.track, period, capture_now64()); /* no exposure before this tick */
    NRF_TIMER4->CC[0] = period;
    NRF_TIMER4->CC[1] = period / 2u;
    NRF_TIMER4->TASKS_CLEAR = 1;
    NRF_TIMER4->TASKS_START = 1;
}

void camera_poll(uint32_t now_ms)
{
    if (!s_cam.running || (uint32_t)(now_ms - s_cam.sampled_ms) < CAM_SAMPLE_MS) { return; }
    uint32_t count;
    uint64_t tick;
    if (!capture_strobe_sample(&count, &tick)) { return; } /* a strobe during the read: next pass */
    s_cam.sampled_ms = now_ms;
    cam_track_anchor(&s_cam.track, strobes(count), tick);
}

static uint8_t health(void)
{
    return (uint8_t)(HEALTH_SYNC_VALID | HEALTH_SKEW_VALID | (board_hfclk_xtal() ? HEALTH_CLOCK_XTAL : 0u));
}

static void reached(uint64_t tick)
{
    if (tick > s_cam.tick) { s_cam.tick = tick; }
}

/* L (crossings may hide in ticks (after, upto]) or K (through tick) of the camera node. */
static int push_record(mq_t *q, uint64_t boot, uint32_t number, uint8_t kind, uint64_t tick, uint64_t upto)
{
    uplink_pl_t u;
    memset(&u, 0, sizeof u);
    u.kind = kind;
    u.flags = health();
    u.ev_seq = (uint16_t)number;
    u.master_boot_id = boot;
    u.capture_seq = s_cam.seq;
    u.tick = tick;
    if (kind == UL_KIND_LOSS) {
        u.capture_seq = u.u.loss.end_seq = s_cam.seq + 1u;
        u.tick = tick + 1u;
        u.u.loss.end_tick = upto;
    }
    if (!mq_push_uplink(q, node_sender_id(), s_cam.session, &u, 0.0f, 0.0f)) { return 0; }
    if (kind == UL_KIND_LOSS) {
        s_cam.seq++;
        if (s_cam.lost != UINT16_MAX) { s_cam.lost++; }
        reached(upto);
    } else {
        reached(tick);
    }
    return 1;
}

/* The record of one C, L or K: 1 queued, 0 not yet known (camwait). */
static int record(const pu_camera_cmd_t *cmd, mq_t *q, uint64_t boot)
{
    const cam_track_t *t = &s_cam.track;
    uint64_t tick, lo, hi, after, upto;
    if (cmd->kind == 'K') {
        tick = cam_track_floor(t, cmd->line);
        s_cam.lag_tick = tick;
        return push_record(q, boot, cmd->q, UL_KIND_CHECKPOINT, tick > s_cam.tick ? tick : s_cam.tick, 0);
    }
    if (cmd->kind == 'C') {
        int m = cam_track_line(t, cmd->line, &tick, &lo, &hi);
        if (m == CAM_LINE_WAIT) { return 0; }
        if (m == CAM_LINE_EXACT) {
            pu_camera_crossing_t c = { boot, s_cam.session, cmd->q, health(), s_cam.seq + 1u, cmd->line, tick,
                                       t->period, lo, hi, cmd->car, cmd->conf };
            if (!mq_push_camera_crossing(q, node_sender_id(), &c)) { return 0; }
            s_cam.seq++;
            reached(tick);
            return 1;
        }
    }
    uint64_t last = cmd->kind == 'L' ? cmd->last : cmd->line;
    if (!cam_track_range(t, cmd->line, last, &after, &upto)) { return 0; }
    return push_record(q, boot, cmd->q, UL_KIND_LOSS, after, upto);
}

void camera_command(const pu_camera_cmd_t *cmd, mq_t *q, uint64_t master_boot_id, uint32_t now_ms)
{
    if (cmd->kind == 'S') {
        camera_stop();
        uint64_t session = 0;
        if (cmd->period && (!entropy_boot_id(&session) || session == 0)) { pu_emit_err("camoff"); return; }
        if (cmd->period) { start(session, cmd->period, now_ms); }
        pu_emit_ack("V");
        return;
    }
    if (!s_cam.running) { pu_emit_err("camoff"); return; }
    if (cmd->q == s_cam.q) { pu_emit_ack("V"); return; } /* a resend of the record taken last */
    uint64_t last = cmd->kind == 'L' ? cmd->last : cmd->line;
    if (cmd->q != s_cam.q + 1u || cmd->line < s_cam.line || (cmd->kind != 'K' && cmd->line == s_cam.line)) {
        pu_emit_err("camseq");
        return;
    }
    if (s_cam.track.broken || last > strobes(capture_strobe_count())) {
        camera_stop();
        pu_emit_err("camline");
        return;
    }
    if (mq_full(q)) { pu_emit_err("camfull"); return; }
    if (!record(cmd, q, master_boot_id)) { pu_emit_err("camwait"); return; }
    s_cam.q = cmd->q;
    s_cam.line = last;
    s_cam.heard_ms = now_ms;
    pu_emit_ack("V");
}

int camera_diag(pu_diag_t *d, uint32_t now_ms)
{
    if (!s_cam.session) { return 0; }
    memset(d, 0, sizeof *d);
    uint32_t age = now_ms - s_cam.heard_ms;
    d->node_id = node_sender_id();
    d->state = !s_cam.running ? PU_STATE_LOST : age <= LINK_OK_MS ? PU_STATE_OK :
               age <= LINK_STALE_MS ? PU_STATE_STALE : PU_STATE_LOST;
    d->last_seen_ms = age;
    if (s_cam.running && s_cam.lag_tick) {
        uint64_t lag = (capture_now64() - s_cam.lag_tick) / TICKS_PER_MS;
        d->lat_ms = lag > UINT32_MAX ? UINT32_MAX : (uint32_t)lag;
    }
    d->provisioned = 1;
    d->health = (uint8_t)((s_cam.running ? (HEALTH_SYNC_VALID | HEALTH_SKEW_VALID) : 0u) |
                          (board_hfclk_xtal() ? HEALTH_CLOCK_XTAL : 0u));
    d->diag_valid = 1;
    d->fifo_drop = s_cam.lost;
    d->sensor_boot_id = s_cam.session;
    return 1;
}
