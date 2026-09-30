#include "proto_usb.h"

#include <string.h>

#include "usb.h"
#include "board.h"
#include "config.h"
#include "crc32.h"
#include "errlog.h"

/* ---- dependency-free line builder ------------------------------------------ */
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

static void lb_i32(lb_t *b, int32_t v)
{
    if (v < 0) { lb_ch(b, '-'); lb_u32(b, (uint32_t)0 - (uint32_t)v); }
    else { lb_u32(b, (uint32_t)v); }
}

/* Two decimals, e.g. -91.50: enough for RSSI (0.5 dB) and SNR (0.25 dB). */
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

static void lb_sp(lb_t *b) { lb_ch(b, ' '); }
static void lb_finish(lb_t *b) { lb_ch(b, '\n'); *b->p = '\0'; }

static void lb_node(lb_t *b, uint32_t node_id, int is_master)
{
    if (is_master) { lb_ch(b, '0'); return; }
    lb_hex8(b, node_id);
}

/* ---- output: control lines retry, periodic lines drop ----------------------- */
#define CTL_QUEUE_LEN 8u
#define CTL_LINE_MAX  128u
#define CTL_MAX_WAIT_MS 1000u

static struct { char line[CTL_LINE_MAX]; uint32_t at_ms; } s_ctl[CTL_QUEUE_LEN];
static unsigned s_ctl_head, s_ctl_count;
static uint32_t s_tx_drop;

static void write_control(const char *line)
{
    if (s_ctl_count == 0 && usb_write(line)) { return; }
    if (s_ctl_count >= CTL_QUEUE_LEN || strlen(line) >= CTL_LINE_MAX) { el_note(EL_USB_DROP); return; }
    unsigned at = (s_ctl_head + s_ctl_count) % CTL_QUEUE_LEN;
    strcpy(s_ctl[at].line, line);
    s_ctl[at].at_ms = board_millis();
    s_ctl_count++;
}

static void write_periodic(const char *line)
{
    if (!usb_write(line) && s_tx_drop != UINT32_MAX) { s_tx_drop++; }
}

void pu_service(uint32_t now_ms)
{
    while (s_ctl_count) {
        if ((uint32_t)(now_ms - s_ctl[s_ctl_head].at_ms) > CTL_MAX_WAIT_MS) {
            el_note(EL_USB_DROP);
        } else if (!usb_write(s_ctl[s_ctl_head].line)) {
            return;
        }
        s_ctl_head = (s_ctl_head + 1u) % CTL_QUEUE_LEN;
        s_ctl_count--;
    }
}

uint32_t pu_tx_drop(void) { return s_tx_drop; }

/* ---- emit helpers ------------------------------------------------------------ */
/* FW_VERSION comes from the Makefile (the release tag in CI). */
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

void pu_emit_identity(uint32_t devid_hi, uint32_t devid_lo, int is_master, uint8_t reset_reason)
{
    char line[112];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "I FSK-WL "); lb_u32(&b, PU_USB_PROTO);
    lb_str(&b, " " FW_VERSION " ");
    lb_hex8(&b, devid_hi); lb_hex8(&b, devid_lo);
    lb_sp(&b); lb_ch(&b, is_master ? 'M' : 'S');
    lb_sp(&b); lb_u32(&b, PROTO_VER);
    lb_sp(&b); lb_f2(&b, LORA_FREQ_MHZ);
    lb_sp(&b); lb_u32(&b, (uint32_t)LORA_SF);
    lb_sp(&b); lb_f2(&b, LORA_BW_KHZ);
    lb_sp(&b); lb_u32(&b, TICKS_PER_MS);
    lb_sp(&b); lb_u32(&b, reset_reason);
    lb_finish(&b);
    write_control(line);
}

void pu_emit_heartbeat(uint64_t now_tick, uint32_t uptime_ms, uint8_t beacon_seq, int nseen)
{
    char line[64];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "H ");
    lb_u64(&b, now_tick);
    lb_sp(&b); lb_u32(&b, uptime_ms);
    lb_sp(&b); lb_u32(&b, beacon_seq);
    lb_sp(&b); lb_u32(&b, (uint32_t)(nseen < 0 ? 0 : nseen));
    lb_finish(&b);
    write_periodic(line);
}

void pu_emit_diag(const pu_diag_t *d)
{
    const char *st = d->state == PU_STATE_OK ? "OK" : (d->state == PU_STATE_STALE ? "STALE" : "LOST");
    char line[240];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "D ");
    lb_node(&b, d->node_id, d->is_master);
    lb_sp(&b); lb_str(&b, st);
    lb_sp(&b); lb_i32(&b, d->skew_ppm);
    lb_sp(&b); lb_u32(&b, d->rx_miss);
    lb_sp(&b); lb_u32(&b, d->beacon_gap);
    lb_sp(&b); lb_u32(&b, d->last_seen_ms);
    lb_sp(&b); lb_f2(&b, d->rssi);
    lb_sp(&b); lb_f2(&b, d->snr);
    lb_sp(&b); lb_u32(&b, d->lat_ms);
    lb_sp(&b); lb_i32(&b, d->temp_c10);
    lb_sp(&b); lb_u32(&b, d->batt_mv);
    lb_sp(&b); lb_u32(&b, d->sec_drop);
    lb_sp(&b); lb_u32(&b, d->provisioned ? 1u : 0u);
    lb_sp(&b); lb_u32(&b, (d->health & HEALTH_SYNC_VALID) ? 1u : 0u);
    lb_sp(&b); lb_u32(&b, (d->health & HEALTH_SKEW_VALID) ? 1u : 0u);
    lb_sp(&b); lb_str(&b, (d->health & HEALTH_CLOCK_XTAL) ? "XTAL" : "RC");
    lb_sp(&b); lb_u32(&b, d->sync_age_ms);
    lb_sp(&b); lb_u32(&b, d->capture_overflow);
    lb_sp(&b); lb_u32(&b, d->fifo_drop);
    lb_sp(&b); lb_u32(&b, d->queue_depth);
    lb_sp(&b); lb_u32(&b, d->queue_overflow);
    lb_sp(&b); lb_u32(&b, d->err_flags);
    lb_sp(&b); lb_u32(&b, d->ver_drop);
    lb_sp(&b); lb_u32(&b, d->tx_drop);
    lb_sp(&b); lb_u32(&b, d->reset_reason);
    lb_sp(&b); lb_u32(&b, d->sensor_boot_id);
    lb_sp(&b); lb_u32(&b, d->master_boot_id);
    lb_finish(&b);
    write_periodic(line);
}

void pu_emit_pps(uint64_t pps_tick, uint32_t utc_s, int32_t ppb, int pps_valid, uint8_t fix,
                 uint8_t sats, uint8_t span_s, uint32_t seg, uint32_t n)
{
    char line[112];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "P ");
    lb_u64(&b, pps_tick);
    lb_sp(&b); lb_u32(&b, utc_s);
    lb_sp(&b); lb_i32(&b, ppb);
    lb_sp(&b); lb_u32(&b, pps_valid ? 1u : 0u);
    lb_sp(&b); lb_u32(&b, fix);
    lb_sp(&b); lb_u32(&b, sats);
    lb_sp(&b); lb_u32(&b, span_s);
    lb_sp(&b); lb_u32(&b, seg);
    lb_sp(&b); lb_u32(&b, n);
    lb_finish(&b);
    write_periodic(line);
}

void pu_emit_clock(const char *token, uint64_t tick, uint32_t master_boot_id)
{
    char line[80];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "T "); lb_str(&b, token);
    lb_sp(&b); lb_u64(&b, tick);
    lb_sp(&b); lb_u32(&b, master_boot_id);
    lb_finish(&b);
    write_control(line);
}

void pu_emit_ack(const char *cmd)
{
    char line[24];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "A "); lb_str(&b, cmd); lb_str(&b, " OK");
    lb_finish(&b);
    write_control(line);
}

void pu_emit_err(const char *reason)
{
    char line[40];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "X "); lb_str(&b, reason);
    lb_finish(&b);
    write_control(line);
}

void pu_emit_err_count(const char *code, uint32_t count)
{
    char line[48];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "X "); lb_str(&b, code);
    lb_sp(&b); lb_u32(&b, count);
    lb_finish(&b);
    write_control(line);
}

void pu_emit_fault(uint32_t pc, uint32_t lr, const char *cause, uint32_t cfsr)
{
    char line[64];
    lb_t b; lb_init(&b, line, sizeof(line));
    lb_str(&b, "X fault ");
    lb_hex8(&b, pc);
    lb_sp(&b); lb_hex8(&b, lr);
    lb_sp(&b); lb_str(&b, cause);
    lb_sp(&b); lb_hex8(&b, cfsr);
    lb_finish(&b);
    write_control(line);
}

/* ---- E lines ------------------------------------------------------------------ */
/* Appends " <crc>\n" over everything written so far. Returns the length, or -1
 * when the line did not fit. */
static int lb_seal(lb_t *b, char *buf, uint32_t *crc)
{
    uint32_t len = (uint32_t)(b->p - buf);
    if (b->p >= b->end) { return -1; }
    *crc = crc32_ieee(buf, len);
    lb_sp(b); lb_hex8(b, *crc);
    if (b->p >= b->end) { return -1; }
    lb_finish(b);
    return (int)(b->p - buf);
}

static void lb_event_head(lb_t *b, uint32_t hseq, uint32_t node_id, int is_master, char kind,
                          uint16_t ev_seq, uint8_t flags, uint32_t master_boot_id, uint32_t sensor_boot_id,
                          uint16_t sync_age_ms, float rssi, float snr, uint32_t capture_seq)
{
    lb_str(b, "E "); lb_u32(b, hseq);
    lb_sp(b); lb_node(b, node_id, is_master);
    lb_sp(b); lb_ch(b, kind);
    lb_sp(b); lb_u32(b, ev_seq);
    lb_sp(b); lb_u32(b, flags);
    lb_sp(b); lb_u32(b, master_boot_id);
    lb_sp(b); lb_u32(b, sensor_boot_id);
    lb_sp(b); lb_u32(b, sync_age_ms);
    lb_sp(b); lb_f2(b, rssi);
    lb_sp(b); lb_f2(b, snr);
    lb_sp(b); lb_u32(b, capture_seq);
}

int pu_format_uplink(char *buf, unsigned cap, uint32_t hseq, uint32_t node_id, uint32_t sensor_boot_id,
                     const uplink_pl_t *u, float rssi, float snr, uint32_t *crc)
{
    /* The master forwards what the sensor sealed; a malformed record (unknown
     * kind, bad count) is formatted as-is so the host quarantines it. */
    char kind = u->kind == UL_KIND_EDGES ? 'C' : u->kind == UL_KIND_LOSS ? 'L' :
                u->kind == UL_KIND_CHECKPOINT ? 'K' : '?';
    lb_t b; lb_init(&b, buf, cap);
    lb_event_head(&b, hseq, node_id, 0, kind, u->ev_seq, u->flags, u->master_boot_id, sensor_boot_id,
                  u->sync_age_ms, rssi, snr, u->capture_seq);
    if (u->kind == UL_KIND_EDGES) {
        unsigned count = u->u.edges.count;
        lb_sp(&b); lb_u32(&b, count);
        uint64_t t = u->tick;
        for (unsigned i = 0; i < count && i < UL_EDGES_MAX; i++) {
            if (i) { t += u->u.edges.dt[i - 1]; }
            lb_sp(&b); lb_u64(&b, t);
        }
    } else if (u->kind == UL_KIND_LOSS) {
        lb_sp(&b); lb_u32(&b, u->u.loss.end_seq);
        lb_sp(&b); lb_u64(&b, u->tick);
        lb_sp(&b); lb_u64(&b, u->u.loss.end_tick);
    } else if (u->kind == UL_KIND_CHECKPOINT) {
        lb_sp(&b); lb_u64(&b, u->tick);
    }
    return lb_seal(&b, buf, crc);
}

int pu_format_timebase_end(char *buf, unsigned cap, uint32_t hseq, uint32_t master_boot_id,
                           uint64_t tick, uint32_t *crc)
{
    lb_t b; lb_init(&b, buf, cap);
    lb_event_head(&b, hseq, NODE_MASTER, 1, 'L', 0, 0, master_boot_id, master_boot_id, 0, 0.0f, 0.0f, 0);
    lb_sp(&b); lb_u32(&b, 0);
    lb_sp(&b); lb_u64(&b, tick);
    lb_sp(&b); lb_u64(&b, tick);
    return lb_seal(&b, buf, crc);
}

/* ---- command parser -------------------------------------------------------- */
static char s_line[80];      /* holds "K " + 64 hex + NUL */
static unsigned s_len;
static uint8_t s_key[32];
static uint32_t s_ack_hseq, s_ack_boot, s_ack_crc;
static char s_clock_token[33];

const uint8_t *pu_setkey(void) { return s_key; }
uint32_t pu_ack_hseq(void) { return s_ack_hseq; }
uint32_t pu_ack_boot(void) { return s_ack_boot; }
uint32_t pu_ack_crc(void) { return s_ack_crc; }
const char *pu_clock_token(void) { return s_clock_token; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* Exactly 64 hex characters into s_key. */
static int parse_key(const char *s)
{
    for (int i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) { return 0; }
        s_key[i] = (uint8_t)((hi << 4) | lo);
    }
    return s[64] == '\0';
}

static int parse_u32(const char **p, uint32_t *out)
{
    uint64_t value = 0;
    const char *s = *p;
    if (*s < '0' || *s > '9') { return 0; }
    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (uint64_t)(*s - '0');
        if (value > UINT32_MAX) { return 0; }
        s++;
    }
    *p = s;
    *out = (uint32_t)value;
    return 1;
}

static int parse_hex8(const char **p, uint32_t *out)
{
    uint32_t v = 0;
    const char *s = *p;
    for (int i = 0; i < 8; i++) {
        int d = hexval(s[i]);
        if (d < 0) { return 0; }
        v = (v << 4) | (uint32_t)d;
    }
    *p = s + 8;
    *out = v;
    return 1;
}

/* C <hseq> <master_boot_id> <crc> */
static int parse_event_ack(const char *s)
{
    s += 2;
    uint32_t hseq, boot, crc;
    if (!parse_u32(&s, &hseq) || *s++ != ' ') { return 0; }
    if (!parse_u32(&s, &boot) || *s++ != ' ') { return 0; }
    if (!parse_hex8(&s, &crc) || *s != '\0') { return 0; }
    s_ack_hseq = hseq;
    s_ack_boot = boot;
    s_ack_crc = crc;
    return 1;
}

static pu_cmd_t classify(const char *s)
{
    if (s[0] == '\0') { return PU_CMD_NONE; }
    if (!strcmp(s, "?ID")) { return PU_CMD_ID; }
    if (!strcmp(s, "?STATUS")) { return PU_CMD_STATUS; }
    if (!strcmp(s, "PING")) { return PU_CMD_PING; }
    if (!strcmp(s, "CP")) { return PU_CMD_CHECKPOINT; }
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
