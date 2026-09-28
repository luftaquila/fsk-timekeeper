#include "proto_usb.h"

#include <string.h>

#include "usb.h"
#include "config.h"

/* ---- dependency-free line builder ------------------------------------------
 * newlib-nano's printf has no %f and no %llu, and we must not truncate 64-bit
 * ticks, so lines are assembled from these primitives instead of snprintf. */
typedef struct { char *p; char *end; } lb_t;

static void lb_init(lb_t *b, char *buf, unsigned n) { b->p = buf; b->end = buf + n - 1; }
static void lb_ch(lb_t *b, char c) { if (b->p < b->end) { *b->p++ = c; } }
static void lb_str(lb_t *b, const char *s) { while (*s) { lb_ch(b, *s++); } }

static void lb_u32(lb_t *b, uint32_t v)
{
    char t[10];
    int n = 0;
    if (v == 0) { t[n++] = '0'; }
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) { lb_ch(b, t[--n]); }
}

static void lb_u64(lb_t *b, uint64_t v)
{
    char t[20];
    int n = 0;
    if (v == 0) { t[n++] = '0'; }
    while (v) { t[n++] = (char)('0' + (uint32_t)(v % 10u)); v /= 10u; }
    while (n) { lb_ch(b, t[--n]); }
}

static void lb_i64(lb_t *b, int64_t v)
{
    if (v < 0) { lb_ch(b, '-'); lb_u64(b, (uint64_t)(-v)); }
    else { lb_u64(b, (uint64_t)v); }
}

static void lb_i32(lb_t *b, int32_t v)
{
    if (v < 0) { lb_ch(b, '-'); lb_u32(b, (uint32_t)(-v)); }
    else { lb_u32(b, (uint32_t)v); }
}

/* Two-decimal fixed point, e.g. -91.50. Enough for RSSI (0.5 dB) / SNR (0.25 dB). */
static void lb_f2(lb_t *b, float v)
{
    int neg = v < 0.0f;
    if (neg) { v = -v; }
    uint32_t s = (uint32_t)(v * 100.0f + 0.5f);
    if (neg && s) { lb_ch(b, '-'); }
    lb_u32(b, s / 100u);
    lb_ch(b, '.');
    uint32_t f = s % 100u;
    lb_ch(b, (char)('0' + f / 10u));
    lb_ch(b, (char)('0' + f % 10u));
}

static void lb_hex8(lb_t *b, uint32_t v)
{
    for (int i = 7; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 0xFu);
        lb_ch(b, (char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

static void lb_finish(lb_t *b) { lb_ch(b, '\n'); *b->p = '\0'; }

/* Node token: the master's self-report uses the literal "0", every sensor uses
 * its 32-bit id as 8 hex chars (so the PC/server identify boards by their id). */
static void lb_node(lb_t *b, uint32_t node_id, int is_master)
{
    if (is_master) { lb_ch(b, '0'); return; }
    lb_hex8(b, node_id);
}

/* ---- emit helpers ---------------------------------------------------------- */
/* Firmware version on the `I` line: FW_VERSION from the Makefile (the release tag in CI). */
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

void pu_emit_identity(uint32_t devid_hi, uint32_t devid_lo)
{
    char line[80];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "I FSK-WL " FW_VERSION " ");
    lb_hex8(&b, devid_hi); lb_hex8(&b, devid_lo);
    lb_ch(&b, ' '); lb_f2(&b, LORA_FREQ_MHZ);
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)LORA_SF);
    lb_ch(&b, ' '); lb_f2(&b, LORA_BW_KHZ);
    lb_str(&b, " 16000");
    lb_finish(&b);
    usb_write(line);
}

void pu_emit_heartbeat(uint64_t now_tick, uint32_t uptime_ms, uint8_t beacon_seq, int nseen)
{
    char line[64];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "H ");
    lb_u64(&b, now_tick);
    lb_ch(&b, ' '); lb_u32(&b, uptime_ms);
    lb_ch(&b, ' '); lb_u32(&b, beacon_seq);
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(nseen < 0 ? 0 : nseen));
    lb_finish(&b);
    usb_write(line);
}

int pu_emit_event(uint32_t node_id, const event_pl_t *event, uint32_t sensor_boot_id, float rssi, float snr)
{
    char line[192];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "E "); lb_node(&b, node_id, node_id == NODE_MASTER);
    lb_ch(&b, ' '); lb_u32(&b, event->ev_seq);
    lb_ch(&b, ' '); lb_u64(&b, event->ev_master_t);
    lb_ch(&b, ' '); lb_u32(&b, event->flags);
    lb_ch(&b, ' '); lb_f2(&b, rssi);
    lb_ch(&b, ' '); lb_f2(&b, snr);
    lb_ch(&b, ' '); lb_u32(&b, event->master_boot_id);
    lb_ch(&b, ' '); lb_u32(&b, sensor_boot_id);
    lb_ch(&b, ' '); lb_u32(&b, event->capture_seq);
    lb_ch(&b, ' '); lb_u32(&b, event->end_seq);
    lb_ch(&b, ' '); lb_u64(&b, event->end_tick);
    lb_ch(&b, ' '); lb_u32(&b, event->sync_age_ms);
    lb_finish(&b);
    return usb_write(line);
}

void pu_emit_diag(uint32_t node_id, int is_master,
                  int state, int64_t offset_tick, int32_t skew_ppm,
                  uint16_t rx_miss, uint16_t beacon_gap, uint32_t last_seen_ms,
                  float rssi, float snr, uint32_t lat_ms,
                  int16_t temp_c10, uint16_t batt_mv,
                  uint32_t sec_drop, int provisioned,
                  int sync_valid, int skew_valid, int clock_xtal, uint16_t sync_age_ms,
                  uint16_t capture_overflow, uint16_t event_drop,
                  uint16_t queue_depth, uint16_t queue_overflow,
                  int usb_ref_valid, int32_t usb_ref_ppm, uint32_t sensor_boot_id, uint32_t master_boot_id)
{
    const char *st = state == PU_STATE_OK ? "OK" : (state == PU_STATE_STALE ? "STALE" : "LOST");
    char line[240];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "D ");
    lb_node(&b, node_id, is_master);
    lb_ch(&b, ' '); lb_str(&b, st);
    lb_ch(&b, ' '); lb_i64(&b, offset_tick);
    lb_ch(&b, ' '); lb_i32(&b, skew_ppm);
    lb_ch(&b, ' '); lb_u32(&b, rx_miss);
    lb_ch(&b, ' '); lb_u32(&b, beacon_gap);
    lb_ch(&b, ' '); lb_u32(&b, last_seen_ms);
    lb_ch(&b, ' '); lb_f2(&b, rssi);
    lb_ch(&b, ' '); lb_f2(&b, snr);
    lb_ch(&b, ' '); lb_u32(&b, lat_ms);
    lb_ch(&b, ' '); lb_i32(&b, temp_c10);
    lb_ch(&b, ' '); lb_u32(&b, batt_mv);
    lb_ch(&b, ' '); lb_u32(&b, sec_drop);          /* security drops (node 0 = master AEAD fails) */
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(provisioned ? 1u : 0u));
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(sync_valid ? 1u : 0u));
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(skew_valid ? 1u : 0u));
    lb_ch(&b, ' '); lb_str(&b, clock_xtal ? "XTAL" : "RC");
    lb_ch(&b, ' '); lb_u32(&b, sync_age_ms);
    lb_ch(&b, ' '); lb_u32(&b, capture_overflow);
    lb_ch(&b, ' '); lb_u32(&b, event_drop);
    lb_ch(&b, ' '); lb_u32(&b, queue_depth);
    lb_ch(&b, ' '); lb_u32(&b, queue_overflow);
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(usb_ref_valid ? 1u : 0u));
    lb_ch(&b, ' '); lb_i32(&b, usb_ref_ppm);
    lb_ch(&b, ' '); lb_u32(&b, sensor_boot_id);
    lb_ch(&b, ' '); lb_u32(&b, master_boot_id);
    lb_finish(&b);
    usb_write(line);
}

void pu_emit_pps(uint64_t pps_tick, uint32_t utc_s, int32_t ppb, int pps_valid,
                 uint8_t fix, uint8_t sats, uint8_t span_s)
{
    char line[80];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "P ");
    lb_u64(&b, pps_tick);
    lb_ch(&b, ' '); lb_u32(&b, utc_s);
    lb_ch(&b, ' '); lb_i32(&b, ppb);
    lb_ch(&b, ' '); lb_u32(&b, (uint32_t)(pps_valid ? 1u : 0u));
    lb_ch(&b, ' '); lb_u32(&b, fix);
    lb_ch(&b, ' '); lb_u32(&b, sats);
    lb_ch(&b, ' '); lb_u32(&b, span_s);
    lb_finish(&b);
    usb_write(line);
}

void pu_emit_clock(const char *token, uint64_t tick, uint32_t master_boot_id)
{
    char line[80];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "T "); lb_str(&b, token);
    lb_ch(&b, ' '); lb_u64(&b, tick);
    lb_ch(&b, ' '); lb_u32(&b, master_boot_id);
    lb_finish(&b);
    usb_write(line);
}

void pu_emit_ack(const char *cmd)
{
    char line[24];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "A "); lb_str(&b, cmd); lb_str(&b, " OK");
    lb_finish(&b);
    usb_write(line);
}

void pu_emit_err(const char *reason)
{
    char line[40];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "X "); lb_str(&b, reason);
    lb_finish(&b);
    usb_write(line);
}

/* ---- command parser -------------------------------------------------------- */
static char s_line[80];      /* must hold "K " + 64 hex + NUL */
static unsigned s_len;
static uint8_t s_key[32];    /* parsed payload of the last K command */
static uint32_t s_ack_node;
static uint16_t s_ack_seq;
static uint64_t s_ack_tick;
static uint32_t s_ack_boot;
static uint32_t s_ack_sensor_boot;
static char s_clock_token[33];

const uint8_t *pu_setkey(void) { return s_key; }
uint32_t pu_event_ack_node(void) { return s_ack_node; }
uint16_t pu_event_ack_seq(void) { return s_ack_seq; }
uint64_t pu_event_ack_tick(void) { return s_ack_tick; }
uint32_t pu_event_ack_boot(void) { return s_ack_boot; }
uint32_t pu_event_ack_sensor_boot(void) { return s_ack_sensor_boot; }
const char *pu_clock_token(void) { return s_clock_token; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* Parse exactly 64 hex chars (NUL-terminated) into s_key. Returns 1 on success. */
static int parse_key(const char *s)
{
    for (int i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) { return 0; }
        s_key[i] = (uint8_t)((hi << 4) | lo);
    }
    return s[64] == '\0'; /* reject trailing junk / wrong length */
}

static int parse_u64(const char **p, uint64_t max, uint64_t *out)
{
    uint64_t value = 0;
    const char *s = *p;
    if (*s < '0' || *s > '9') { return 0; }
    while (*s >= '0' && *s <= '9') {
        uint8_t digit = (uint8_t)(*s - '0');
        if (value > (max - digit) / 10u) { return 0; }
        value = value * 10u + digit;
        s++;
    }
    *p = s;
    *out = value;
    return 1;
}

static int parse_event_ack(const char *s)
{
    if (s[0] != 'C' || s[1] != ' ') { return 0; }
    s += 2;
    uint32_t node = 0;
    if (s[0] == '0' && s[1] == ' ') {
        s++;
    } else {
        for (int i = 0; i < 8; i++) {
            int digit = hexval(*s++);
            if (digit < 0) { return 0; }
            node = (node << 4) | (uint32_t)digit;
        }
    }
    if (*s++ != ' ') { return 0; }
    uint64_t seq;
    if (!parse_u64(&s, UINT16_MAX, &seq) || *s++ != ' ') { return 0; }
    uint64_t tick;
    if (!parse_u64(&s, UINT64_MAX, &tick) || *s++ != ' ') { return 0; }
    uint64_t boot;
    if (!parse_u64(&s, UINT32_MAX, &boot) || *s++ != ' ') { return 0; }
    uint64_t sensor_boot;
    if (!parse_u64(&s, UINT32_MAX, &sensor_boot) || *s != '\0') { return 0; }
    s_ack_sensor_boot = (uint32_t)sensor_boot;
    s_ack_node = node;
    s_ack_seq = (uint16_t)seq;
    s_ack_tick = tick;
    s_ack_boot = (uint32_t)boot;
    return 1;
}

static pu_cmd_t classify(const char *s)
{
    if (s[0] == '\0') { return PU_CMD_NONE; } /* blank line — ignore */
    if (!strcmp(s, "?ID")) { return PU_CMD_ID; }
    if (!strcmp(s, "?STATUS")) { return PU_CMD_STATUS; }
    if (!strcmp(s, "PING")) { return PU_CMD_PING; }
    if (s[0] == 'K' && s[1] == ' ') { return parse_key(s + 2) ? PU_CMD_SETKEY : PU_CMD_BAD; }
    if (s[0] == 'C' && s[1] == ' ') { return parse_event_ack(s) ? PU_CMD_EVENT_ACK : PU_CMD_BAD; }
    if (s[0] == 'T' && s[1] == ' ' && strlen(s + 2) == 32u) {
        for (unsigned i = 0; i < 32u; i++) {
            if (hexval(s[i + 2]) < 0) { return PU_CMD_BAD; }
        }
        memcpy(s_clock_token, s + 2, 33u);
        return PU_CMD_CLOCK;
    }
    return PU_CMD_BAD;
}

pu_cmd_t pu_feed(int c)
{
    if (c == '\n' || c == '\r') {
        s_line[s_len] = '\0';
        s_len = 0;
        return classify(s_line);
    }
    if (s_len < sizeof(s_line) - 1) {
        s_line[s_len++] = (char)c;
    }
    return PU_CMD_NONE;
}
