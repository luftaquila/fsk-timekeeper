/* MAC simulator: the production MAC, sync, FIFO, registry, host queue and line formats on fake
 * air (airtime, collisions, loss, half duplex), TIMER1, ppm clocks and an acking USB host; one
 * process per scenario. Invariants checked:
 *   - every TX sits in its slot window: DATA in a slot dealt to that sensor, JOIN in a REQUEST
 *     slot, OFFER/CONFIRM in their admission slots; none overlaps another TX or a beacon, except
 *     JOINs with each other (check_slots)
 *   - every capture reaches the host exactly once and in order, or is covered by a loss range
 *   - stamps are within a few ticks of the truth, interpolated ones after a beacon gap too
 *   - checkpoints never claim past an undelivered capture
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/capture.h"
#include "../src/config.h"
#include "../src/crc32.h"
#include "../src/errlog.h"
#include "../src/mac_master.h"
#include "../src/mac_sensor.h"
#include "../src/meas.h"
#include "../src/proto_usb.h"
#include "../src/radio.h"
#include "../src/secure.h"
#include "../src/usb.h"
#include "../src/board.h"
#include "../src/mac_radio.h"
#include "../src/mac_schedule.h"

#define SIM_NODES   7
#define AIR_MAX     200000
#define REC_MAX     20000
#define EDGE_MAX    20000
#define STEP_US     200u
/* Conservative oscillator startup allowance, not a measured XTAL delay. */
#define OSC_START_BUDGET_US 5000u
#define CAD_US      2560u   /* 4 symbols at SF7/BW250 */
static_assert(OSC_START_BUDGET_US + CAD_US < CAD_DONE_MS * 1000u, "a modelled CAD is a healthy one");
#define TX_SETUP_US (TX_SETUP_MS * 1000u) /* SPI (~1.5 ms at 1 MHz) + oscillator + PA ramp */
#define RX_SETUP_US 5500u   /* SPI + oscillator before the receiver listens */
#define HEADER_US   10400u  /* preamble + header before HEADER_VALID */

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

/* ---- nodes and air --------------------------------------------------------- */
typedef struct {
    int src;
    uint64_t start_us, end_us;
    int len;
    uint8_t data[WIRE_MAX], plain[WIRE_MAX];
    uint32_t seen;       /* receivers that evaluated it */
} tx_t;

typedef struct {
    int idx, is_master;
    uint32_t sender_id, tx_ctr, rng;
    uint64_t boot_id, tx_end;
    int transmitting;
    int reject_tx_once;
    uint32_t deadline;
    uint8_t sealed_plain[WIRE_MAX];
    int32_t ppm;
    uint64_t tick_base;
    uint64_t now_us;
    int xtal;
    uint32_t hfxo_stops;
    int rx_on;
    uint64_t rx_since_us;
    int have_pkt, pkt_len;
    uint8_t pkt[WIRE_MAX];
    int dio1;
    uint64_t dio1_tick;
    int noresp;
    int init_pending, configured, begin_fail, rx_fail;
    unsigned receive_calls;
    unsigned begin_calls;
    unsigned cad_skips, lbt_skips;
    unsigned loss_pct;     /* receive loss, percent */
    int disabled, drop_type;
    int deaf;              /* drops every beacon */
    uint64_t jam_from_us, jam_to_us; /* a strong stranger nearby: every RSSI listen is loud */
    struct { uint64_t tick; uint32_t seq; int xtal; } ring[64];
    unsigned rh, rt;
    uint32_t cseq;
    int loss_pending;
    uint32_t loss_first, loss_last;
    uint64_t loss_first_tick, loss_last_tick;
    uint16_t overflow;
    mac_sensor_t s;
} node_t;

static tx_t air[AIR_MAX];
static unsigned air_n;
static void isr_catch_up(node_t *n);
static node_t nodes[SIM_NODES];
static int n_nodes;
static node_t *cur;
static uint64_t now_global;
static uint32_t seal_delay_us;
static uint32_t down_delay_us, up_delay_us, admission_delay_us;
static int cad_error;
static int lbt_on = RADIO_DEF_LBT;
static mq_t mq;
static mac_master_t mm;

static uint64_t local_tick(const node_t *n, uint64_t true_us)
{
    uint64_t t = true_us * 16u;
    int64_t drift = (int64_t)t * n->ppm / 1000000;
    return n->tick_base + t + (uint64_t)drift;
}

static uint32_t airtime_us(int len) { return LORA_TOA_US((unsigned)len); }

static int is_beacon(const tx_t *x) { return SEC_VT_TYPE(x->data[0]) == PKT_TYPE_BEACON; }

/* DIO1 receive latency, independent of TxDone and receiver clock; its length part is symmetric. */
static uint64_t receive_done(const tx_t *x)
{
    return x->end_us + (x->src == 0 ? down_delay_us : up_delay_us) +
           (x->len == WIRE_ADMISSION ? admission_delay_us : 0u);
}

static int collided(unsigned i)
{
    const tx_t *x = &air[i];
    unsigned lo = i > 64u ? i - 64u : 0u;
    unsigned hi = air_n < i + 64u ? air_n : i + 64u;
    for (unsigned j = lo; j < hi; j++) {
        if (j != i && air[j].start_us < x->end_us && air[j].end_us > x->start_us) { return 1; }
    }
    return 0;
}

/* Hand the receiver every packet that ended while it listened, collision-free. */
static void deliver(node_t *r)
{
    unsigned from = air_n > 400u ? air_n - 400u : 0u;
    for (unsigned i = from; i < air_n; i++) {
        tx_t *x = &air[i];
        if (x->src == r->idx || (x->seen >> r->idx) & 1u || receive_done(x) > r->now_us) { continue; }
        x->seen |= 1u << r->idx;
        if (!r->rx_on || r->rx_since_us > x->start_us || collided(i)) { continue; }
        if (mix(i * 131u + (unsigned)r->idx) % 100u < r->loss_pct) { continue; }
        if (r->deaf && is_beacon(x)) { continue; }
        if (r->drop_type && (unsigned)r->drop_type == SEC_VT_TYPE(x->data[0])) { continue; }
        memcpy(r->pkt, x->data, (size_t)x->len);
        r->pkt_len = x->len;
        r->have_pkt = 1;
        r->dio1 = 1;
        r->dio1_tick = local_tick(r, receive_done(x));
    }
}

static const tx_t *on_air(const node_t *r)
{
    unsigned from = air_n > 64u ? air_n - 64u : 0u;
    for (unsigned i = from; i < air_n; i++) {
        const tx_t *x = &air[i];
        if (x->src != r->idx && x->start_us <= r->now_us && receive_done(x) > r->now_us) { return x; }
    }
    return NULL;
}

/* ---- fake hardware boundary ------------------------------------------------ */
uint64_t capture_now64(void) { return local_tick(cur, cur->now_us); }
uint32_t board_millis(void) { return (uint32_t)(cur->now_us / 1000u); }
uint32_t board_micros(void) { return (uint32_t)cur->now_us; }
void board_delay_ms(uint32_t ms) { cur->now_us += (uint64_t)ms * 1000u; }
int board_hfclk_xtal(void) { return cur->xtal; }
uint32_t board_hfxo_stops(void) { return cur->hfxo_stops; }
void board_led_toggle(void) {}
void meas_service(uint32_t now) { (void)now; }
int16_t meas_temp_c10(void) { return 250; }
uint16_t meas_vddh_mv(void) { return 3900; }

int capture_dio1_get(uint64_t *tick)
{
    if (!cur->dio1) { return 0; }
    cur->dio1 = 0;
    *tick = cur->dio1_tick;
    return 1;
}

static void isr_edge(node_t *n, uint64_t true_us)
{
    uint32_t seq = ++n->cseq;
    uint64_t tick = local_tick(n, true_us);
    unsigned next = (n->rh + 1u) & 63u;
    if (next == n->rt || n->loss_pending) {
        n->overflow++;
        if (!n->loss_pending) { n->loss_first = seq; n->loss_first_tick = tick; }
        n->loss_last = seq; n->loss_last_tick = tick; n->loss_pending = 1;
    } else {
        n->ring[n->rh].tick = tick;
        n->ring[n->rh].seq = seq;
        n->ring[n->rh].xtal = n->xtal;
        n->rh = next;
    }
}

int capture_sensor_get(uint64_t *tick, uint32_t *seq, int *clock_xtal)
{
    if (cur->rt == cur->rh) { return 0; }
    *tick = cur->ring[cur->rt].tick;
    *seq = cur->ring[cur->rt].seq;
    *clock_xtal = cur->ring[cur->rt].xtal;
    cur->rt = (cur->rt + 1u) & 63u;
    return 1;
}

int capture_sensor_loss(uint64_t *first_tick, uint64_t *last_tick, uint32_t *first_seq, uint32_t *last_seq)
{
    if (!cur->loss_pending) { return 0; }
    *first_tick = cur->loss_first_tick; *last_tick = cur->loss_last_tick;
    *first_seq = cur->loss_first; *last_seq = cur->loss_last;
    cur->loss_pending = 0;
    return 1;
}

int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq)
{
    *tick = capture_now64();
    *seq = cur->cseq;
    return cur->rt == cur->rh && !cur->loss_pending;
}

uint16_t capture_sensor_overflow(void) { return cur->overflow; }

int radio_begin(void)
{
    deliver(cur);
    cur->begin_calls++;
    if (getenv("SIM_TRACE")) fprintf(stderr,"reset node%d at%llu pending%d noresp%d\n",cur->idx,(unsigned long long)cur->now_us,cur->init_pending,cur->noresp);
    cur->init_pending = 1;
    cur->configured = 0;
    cur->rx_on = 0;
    cur->have_pkt = 0;
    if (cur->begin_fail) { return -1; }
    cur->noresp = 0;
    cur->configured = 1;
    return 0;
}

int radio_start_rx(void)
{
    /* The hardware HAL rejects SPI access after the caller's deadline. */
    assert(!cur->deadline || (int32_t)(cur->deadline - board_micros()) > 0);
    deliver(cur);
    if (cur->rx_fail) { return -1; }
    if (cur->configured) { cur->init_pending = 0; }
    cur->rx_on = 1;
    cur->rx_since_us = cur->now_us + RX_SETUP_US;
    cur->have_pkt = 0; /* startReceive clears the IRQ status: an unread packet is gone */
    return 0;
}

int radio_standby(void)
{
    deliver(cur);
    cur->rx_on = 0;
    return 0;
}

void radio_tx_abort(void) { cur->transmitting = 0; cur->init_pending = 1; cur->configured = 0; cur->rx_on = 0; }
void radio_limit(uint32_t end_us) { cur->deadline = end_us; }
void radio_unlimit(void) { cur->deadline = 0; }
int radio_tx_start(const uint8_t *data, int len, uint32_t end_us)
{
    if (cur->reject_tx_once) { cur->reject_tx_once = 0; return -1; }
    if (cur->transmitting || radio_needs_reset() ||
        (int32_t)(end_us - board_micros()) < (int32_t)(TX_SETUP_US + airtime_us(len))) { return -1; }
    deliver(cur); cur->rx_on = 0; cur->have_pkt = 0;
    assert(air_n < AIR_MAX);
    tx_t *x = &air[air_n++];
    x->src = cur->idx; x->start_us = cur->now_us + TX_SETUP_US;
    x->end_us = x->start_us + airtime_us(len); x->len = len; x->seen = 0;
    memcpy(x->data, data, (size_t)len); memcpy(x->plain, cur->sealed_plain, sizeof(x->plain));
    cur->now_us = x->start_us; cur->tx_end = x->end_us; cur->transmitting = 1;
    cur->deadline = end_us;
    return 0;
}
int radio_tx_poll(void)
{
    if (!cur->transmitting) { return -1; }
    if (cur->now_us < cur->tx_end) { return 0; }
    cur->transmitting = 0;
    cur->dio1 = 1; cur->dio1_tick = local_tick(cur, cur->tx_end);
    radio_unlimit(); radio_start_rx(); return 1;
}

int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr)
{
    cur->receive_calls++;
    deliver(cur);
    if (!cur->have_pkt) { return 0; }
    int len = cur->pkt_len < maxlen ? cur->pkt_len : maxlen;
    memcpy(buf, cur->pkt, (size_t)len);
    if (rssi) { *rssi = -70.0f; }
    if (snr) { *snr = 8.0f; }
    radio_start_rx();
    return len;
}

int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms)
{
    deliver(cur);
    if (cur->have_pkt) { return RADIO_RX_PACKET; }
    const tx_t *x = on_air(cur);
    if (!x || !cur->rx_on || cur->rx_since_us > x->start_us) { return RADIO_RX_IDLE; }
    uint64_t header_at = x->start_us + HEADER_US;
    if (cur->now_us < header_at) {
        if (cur->now_us + (uint64_t)header_wait_ms * 1000u < header_at) {
            cur->now_us += (uint64_t)header_wait_ms * 1000u;
            return RADIO_RX_IDLE;
        }
        cur->now_us = header_at;
    }
    if (max_ms == 0 || receive_done(x) > cur->now_us + (uint64_t)max_ms * 1000u) {
        if (max_ms) { cur->now_us += (uint64_t)max_ms * 1000u; }
        return RADIO_RX_BUSY;
    }
    cur->now_us = receive_done(x);
    deliver(cur);
    return cur->have_pkt ? RADIO_RX_PACKET : RADIO_RX_IDLE;
}

int radio_cad(void)
{
    if (cad_error) { cur->now_us += CAD_US; return RADIO_CAD_ERROR; }
    deliver(cur);
    uint64_t t0 = cur->now_us + OSC_START_BUDGET_US; /* listens only after the oscillator start */
    int32_t remaining = (int32_t)(cur->deadline - board_micros());
    if (cur->deadline && remaining < (int32_t)(OSC_START_BUDGET_US + CAD_US)) {
        if (remaining <= 0) { return RADIO_CAD_BUSY; } /* RX never left */
        cur->now_us = t0 + CAD_US; /* the scan is awaited to completion, then cancelled */
        uint32_t access = cur->deadline;
        radio_limit(board_micros() + TX_SETUP_MS * 1000u);
        int result = radio_start_rx();
        radio_limit(access);
        cur->cad_skips++;
        return result == 0 ? RADIO_CAD_BUSY : RADIO_CAD_ERROR;
    }
    cur->now_us = t0 + CAD_US;
    unsigned from = air_n > 64u ? air_n - 64u : 0u;
    for (unsigned i = from; i < air_n; i++) {
        if (air[i].src != cur->idx && air[i].start_us < cur->now_us && air[i].end_us > t0) {
            cur->rx_on = 1;
            cur->rx_since_us = cur->now_us + RX_SETUP_US;
            cur->have_pkt = 0;
            return 1;
        }
    }
    cur->rx_on = 0;
    return 0;
}

int radio_needs_reset(void) { return cur->init_pending || cur->noresp >= (int)RADIO_NORESP_RESET; }

int radio_lbt_enabled(void) { return lbt_on; }

/* The nodes stand close together, so any other node on air is loud, like a jammer. */
int radio_lbt_listen(void)
{
    deliver(cur);
    uint64_t t0 = cur->now_us;
    int32_t remaining = (int32_t)(cur->deadline - board_micros());
    if (cur->deadline && remaining <= (int32_t)(LBT_LISTEN_MS * 1000u)) {
        if (remaining > 0) { cur->now_us += remaining; }
        isr_catch_up(cur);
        cur->lbt_skips++;
        return RADIO_LBT_LOUD;
    }
    cur->now_us = t0 + LBT_LISTEN_MS * 1000u;
    isr_catch_up(cur); /* the GPIOTE ISR keeps capturing while the main loop listens */
    if (cur->jam_from_us < cur->now_us && cur->jam_to_us > t0) { return RADIO_LBT_LOUD; }
    unsigned from = air_n > 64u ? air_n - 64u : 0u;
    for (unsigned i = from; i < air_n; i++) {
        if (air[i].src != cur->idx && air[i].start_us < cur->now_us && air[i].end_us > t0) { return RADIO_LBT_LOUD; }
    }
    return RADIO_LBT_QUIET;
}

/* Real production AEAD and KDF; only boot/counter/RNG hardware is per-node. */
#define sec_seal real_sec_seal
#define sec_seal_key real_sec_seal_key
#define sec_seal_uplink real_sec_seal_uplink
#define sec_boot_id real_sec_boot_id
#define sec_random real_sec_random
#define sec_challenge real_sec_challenge
#define sec_provisioned real_sec_provisioned
#include "../src/secure.c"
#undef sec_seal
#undef sec_seal_key
#undef sec_seal_uplink
#undef sec_boot_id
#undef sec_random
#undef sec_challenge
#undef sec_provisioned
int keystore_load(uint8_t out[KEYSTORE_KEY_LEN]) { memset(out, 0x42, KEYSTORE_KEY_LEN); return 1; }
int entropy_boot_id(uint64_t *out) { *out = cur->boot_id; return 1; }
int entropy_poll(uint8_t *out) { *out = (uint8_t)sec_random(); return 1; }
uint64_t sec_boot_id(void) { return cur->boot_id; }
int sec_provisioned(void) { return 1; }
uint32_t sec_random(void) { cur->rng = mix(cur->rng + 0x9E3779B9u); return cur->rng; }
int sec_challenge(uint8_t out[16]) {
    for (unsigned i = 0; i < 4; i++) { uint32_t r = sec_random(); memcpy(out + i*4, &r, 4); }
    return 1;
}
static void seal_context(const void *payload, int len, uint8_t type)
{
    if (type == PKT_TYPE_UPLINK && cur->idx == 1) { cur->now_us += seal_delay_us; }
    g_boot_id = cur->boot_id; g_tx_ctr = cur->tx_ctr; g_have_boot_id = g_provisioned = 1;
    memset(KEY, 0x42, sizeof(KEY));
    memset(cur->sealed_plain, 0, sizeof(cur->sealed_plain));
    memcpy(cur->sealed_plain, payload, (size_t)len);
}
int sec_seal(uint8_t *out, int cap, uint8_t type, uint32_t id, const void *p, int len)
{
    seal_context(p, len, type);
    int r = real_sec_seal(out, cap, type, id, p, len); cur->tx_ctr = g_tx_ctr; return r;
}
int sec_seal_key(const uint8_t key[32], uint8_t *out, int cap, uint8_t type, uint32_t id, const void *p, int len)
{
    seal_context(p, len, type);
    int r = real_sec_seal_key(key, out, cap, type, id, p, len); cur->tx_ctr = g_tx_ctr; return r;
}
int sec_seal_uplink(const uint8_t key[32], uint8_t *out, int cap, uint32_t id, const void *p, int len, int wire_len)
{
    seal_context(p, len, PKT_TYPE_UPLINK);
    int r = real_sec_seal_uplink(key, out, cap, id, p, len, wire_len); cur->tx_ctr = g_tx_ctr; return r;
}

/* ---- host: parses E lines, checks their crc, acks ------------------------ */
typedef struct {
    uint32_t hseq, node;
    uint64_t sboot, mboot;
    char kind;
    uint32_t seq, end_seq;
    uint64_t tick, end_tick;
    unsigned flags;
    uint64_t at_us;
} rec_t;

static rec_t recs[REC_MAX];
static unsigned n_recs;
static char inbox[64][PU_LINE_MAX + 8];
static unsigned inbox_n;
static int host_acks = 1;
static uint32_t last_hseq;
static unsigned d_lines;
static char last_d[PU_LINE_MAX + 8];
static int track_timing;
static struct { uint64_t at_us; unsigned age, valid; } timing_report[SIM_NODES];

int usb_console_ready(void) { return 1; }
uint32_t usb_console_epoch(void) { return 0; }
int usb_write(const char *s)
{
    if (s[0] == 'D') { d_lines++; strcpy(last_d, s); }
    if (s[0] == 'D' && track_timing) {
        char copy[PU_LINE_MAX + 8], *fields[40];
        strcpy(copy, s);
        unsigned count = 0;
        for (char *p = strtok(copy, " \n"); p && count < 40; p = strtok(NULL, " \n")) { fields[count++] = p; }
        assert(count == 33);
        uint32_t id = (uint32_t)strtoul(fields[1], NULL, 16);
        for (int i = 1; i < n_nodes; i++) if (nodes[i].sender_id == id) {
            timing_report[i].at_us = cur->now_us;
            timing_report[i].age = (unsigned)strtoul(fields[17], NULL, 10);
            timing_report[i].valid = !strcmp(fields[14], "1") && !strcmp(fields[15], "1") &&
                                     !strcmp(fields[16], "XTAL") && !strcmp(fields[32], "1");
        }
    }
    if (s[0] != 'E') { return 1; }
    assert(inbox_n < 64);
    strcpy(inbox[inbox_n++], s);
    return 1;
}
int usb_read_byte(void) { return -1; }

static void host_line(const char *line)
{
    const char *sp = strrchr(line, ' ');
    assert(sp && strlen(sp + 1) == 9);
    uint32_t crc = (uint32_t)strtoul(sp + 1, NULL, 16);
    assert(crc == crc32_ieee(line, (uint32_t)(sp - line)));
    char buf[PU_LINE_MAX + 8];
    strcpy(buf, line);
    char *tok[32];
    int nt = 0;
    for (char *p = strtok(buf, " \n"); p && nt < 32; p = strtok(NULL, " \n")) { tok[nt++] = p; }
    assert(nt >= 14 && !strcmp(tok[0], "E"));
    uint32_t hseq = (uint32_t)strtoul(tok[1], NULL, 10);
    uint64_t mboot = strtoull(tok[6], NULL, 10);
    if (host_acks) { (void)mq_ack(&mq, hseq, mboot, crc); }
    if (hseq <= last_hseq) { return; } /* a re-send */
    assert(hseq == last_hseq + 1u);    /* nothing skipped */
    last_hseq = hseq;
    rec_t r = { .hseq = hseq, .node = !strcmp(tok[2], "0") ? 0u : (uint32_t)strtoul(tok[2], NULL, 16),
                .kind = tok[3][0], .flags = (unsigned)strtoul(tok[5], NULL, 10), .mboot = mboot,
                .sboot = strtoull(tok[7], NULL, 10), .seq = (uint32_t)strtoul(tok[11], NULL, 10),
                .at_us = now_global };
    if (r.kind == 'C') {
        unsigned count = (unsigned)strtoul(tok[12], NULL, 10);
        assert(count >= 1 && count <= UL_EDGES_MAX && nt == 14 + (int)count);
        for (unsigned i = 0; i < count; i++) {
            rec_t e = r;
            e.seq = r.seq + i;
            e.end_seq = e.seq;
            e.tick = e.end_tick = strtoull(tok[13 + i], NULL, 10);
            if (i) { assert(e.tick > recs[n_recs - 1].tick); }
            assert(n_recs < REC_MAX);
            recs[n_recs++] = e;
        }
    } else if (r.kind == 'L') {
        assert(nt == 16);
        r.end_seq = (uint32_t)strtoul(tok[12], NULL, 10);
        r.tick = strtoull(tok[13], NULL, 10);
        r.end_tick = strtoull(tok[14], NULL, 10);
        recs[n_recs++] = r;
    } else {
        assert(r.kind == 'K' && nt == 24);
        r.end_seq = r.seq;
        r.tick = r.end_tick = strtoull(tok[12], NULL, 10);
        recs[n_recs++] = r;
    }
}

/* ---- simulation --------------------------------------------------------- */
typedef struct { int node; uint64_t us; int injected; uint32_t seq; } edge_t;
static edge_t edges[EDGE_MAX];
static unsigned n_edges, next_edge;

static void inject(edge_t *e)
{
    isr_edge(&nodes[e->node], e->us);
    e->injected = 1;
    e->seq = nodes[e->node].cseq;
}

/* Edges of n up to its own clock, ahead of the global step (n blocked in a radio call). */
static void isr_catch_up(node_t *n)
{
    for (unsigned i = next_edge; i < n_edges && edges[i].us <= n->now_us; i++) {
        if (edges[i].node == n->idx && !edges[i].injected) { inject(&edges[i]); }
    }
}

static void add_node(int is_master, uint32_t id, int32_t ppm, unsigned loss_pct)
{
    node_t *n = &nodes[n_nodes];
    memset(n, 0, sizeof(*n));
    n->idx = n_nodes++;
    n->is_master = is_master;
    n->sender_id = id;
    n->boot_id = (UINT64_C(0x12345678) << 32) | mix(id * 7919u + 17u);
    n->rng = mix(id);
    n->ppm = ppm;
    n->tick_base = (uint64_t)mix(id) * 1000u;
    n->xtal = 1;
    n->configured = 1;
    n->loss_pct = loss_pct;
    n->now_us = now_global + (uint64_t)n->idx * 137u; /* sensors do not start in lockstep */
    cur = n;
    g_have_boot_id = g_provisioned = 1; memset(KEY, 0x42, sizeof(KEY));
    if (is_master) {
        mq_init(&mq);
        mac_master_init(&mm, &mq);
    } else {
        mac_sensor_init(&n->s, id, 0);
    }
}

static void edge_at(int node, uint64_t us)
{
    assert(n_edges < EDGE_MAX);
    edges[n_edges++] = (edge_t){ .node = node, .us = us };
}

typedef void (*hook_t)(uint64_t us);

static int cmp_edge(const void *a, const void *b)
{
    const edge_t *x = a, *y = b;
    return x->us < y->us ? -1 : x->us > y->us;
}

static void run(uint64_t end_us, hook_t hook)
{
    next_edge = 0;
    qsort(edges, n_edges, sizeof(edges[0]), cmp_edge);
    for (; now_global < end_us; now_global += STEP_US) {
        if (hook) { hook(now_global); }
        while (next_edge < n_edges && edges[next_edge].us <= now_global) {
            edge_t *e = &edges[next_edge++];
            if (!e->injected) { inject(e); }
        }
        for (int i = 0; i < n_nodes; i++) {
            node_t *n = &nodes[i];
            if (n->disabled || n->now_us > now_global) { continue; }
            n->now_us = now_global;
            cur = n;
            if (n->is_master) {
                if (mac_master_step(&mm) && track_timing) { mac_master_report(&mm, 1); }
                mq_pump(&mq, board_millis());
                for (unsigned k = 0; k < inbox_n; k++) { host_line(inbox[k]); }
                inbox_n = 0;
            } else {
                mac_sensor_step(&n->s);
            }
        }
    }
}

/* ---- invariant checks ----------------------------------------------------- */
static void check_slots(uint64_t from_us)
{
    uint64_t last_beacon_end = 0;
    uint32_t beacon_ctr = 0;
    int beacon_len = 0;
    admission_beacon_t b = {0}; uint8_t slots[SLOT_COUNT] = {0};
    for (unsigned i = 0; i < air_n; i++) {
        const tx_t *x = &air[i];
        if (is_beacon(x)) {
            last_beacon_end = x->end_us; memcpy(&b, x->plain, sizeof(b));
            beacon_ctr = sec_wire_counter(x->data); beacon_len = x->len;
            if (b.base.flags & BEACON_ADMISSION) { memcpy(slots, b.schedule, sizeof(slots)); }
            else { mac_normal_schedule(&b.base, slots); }
            assert(mac_schedule_valid(slots)); continue;
        }
        if (x->start_us < from_us || !last_beacon_end) { continue; }
        uint64_t off = x->start_us - last_beacon_end;
        assert(off >= SLOT_OFFSET_MS * 1000u);
        uint64_t k = (off - SLOT_OFFSET_MS * 1000u) / (SLOT_LEN_MS * 1000u);
        assert(k < SLOT_COUNT);
        uint64_t slot_start = last_beacon_end + (SLOT_OFFSET_MS + k * SLOT_LEN_MS) * 1000u;
        assert(x->start_us - slot_start <= (SLOT_LATE_MS + 1u) * 1000u + TX_SETUP_US);
        assert(x->end_us <= slot_start + (SLOT_LEN_MS - SLOT_GUARD_MS - LBT_LISTEN_MS) * 1000u);
        unsigned role = SLOT_ROLE(slots[k]), entry = SLOT_ENTRY(slots[k]);
        unsigned type = SEC_VT_TYPE(x->data[0]);
        uint32_t id = nodes[x->src].sender_id;
        if (type == PKT_TYPE_UPLINK) {
            const uplink_pl_t *u = (const uplink_pl_t *)x->plain;
            assert(role == SLOT_ROLE_DATA && b.base.slot[entry].node_id == id);
            assert(x->len == beacon_len && u->beacon_ctr == beacon_ctr);
        }
        else if (type == PKT_TYPE_JOIN) { assert(role == SLOT_ROLE_REQUEST); }
        else if (type == PKT_TYPE_CONFIRM) { assert(role == SLOT_ROLE_CONFIRM && b.base.slot[entry].node_id == id); }
        else { assert(type == PKT_TYPE_OFFER && role == SLOT_ROLE_OFFER && x->src == 0); }
        for (unsigned j = 0; j < air_n; j++) {
            if (j == i || (type == PKT_TYPE_JOIN && SEC_VT_TYPE(air[j].data[0]) == PKT_TYPE_JOIN)) { continue; }
            assert(!(air[j].start_us < x->end_us && air[j].end_us > x->start_us));
        }
    }
}

/* Each capture of `node` after `from_us`: once, in order, within tol ticks or in a loss range. */
static void check_records(int node, uint64_t from_us, int64_t tol, unsigned *interp, unsigned *lost)
{
    const node_t *n = &nodes[node];
    const node_t *m = &nodes[0];
    uint32_t prev_seq = 0;
    int have_prev = 0;
    unsigned idx = 0;
    *interp = *lost = 0;
    for (unsigned i = 0; i < n_recs; i++) {
        const rec_t *r = &recs[i];
        if (r->node != n->sender_id || r->kind == 'K') { continue; }
        if (have_prev) { assert((int32_t)(r->seq - prev_seq) > 0); }
        prev_seq = r->kind == 'L' ? r->end_seq : r->seq;
        have_prev = 1;
    }
    for (unsigned i = 0; i < n_edges; i++) {
        const edge_t *e = &edges[i];
        if (e->node != node || e->us < from_us) { continue; }
        assert(e->injected);
        int found = 0;
        for (unsigned j = 0; j < n_recs; j++) {
            const rec_t *r = &recs[j];
            if (r->node != n->sender_id || r->kind == 'K') { continue; }
            if (r->kind == 'C' && r->seq == e->seq) {
                assert(!found);
                found = 1;
                int64_t err = (int64_t)(r->tick - local_tick(m, e->us));
                if (err < -tol || err > tol) {
                    fprintf(stderr, "node %d seq %u err %lld flags %u\n", node, e->seq, (long long)err, r->flags);
                    abort();
                }
                if (r->flags & EVENT_INTERPOLATED) { (*interp)++; }
            } else if (r->kind == 'L' && (int32_t)(e->seq - r->seq) >= 0 && (int32_t)(r->end_seq - e->seq) >= 0) {
                assert(!found);
                found = 2;
                (*lost)++;
            }
        }
        if (!found) { fprintf(stderr, "node %d seq %u (t=%llu us) never arrived\n", node, e->seq, (unsigned long long)e->us); abort(); }
        idx++;
    }
    assert(idx > 0);
}

/* A checkpoint claims "through tick, the last capture is seq". */
static void check_checkpoints(int node)
{
    const node_t *n = &nodes[node];
    const node_t *m = &nodes[0];
    unsigned cps = 0;
    for (unsigned j = 0; j < n_recs; j++) {
        const rec_t *r = &recs[j];
        if (r->node != n->sender_id || r->kind != 'K' || (r->flags & 7u) != 7u) { continue; }
        cps++;
        for (unsigned i = 0; i < n_edges; i++) {
            const edge_t *e = &edges[i];
            if (e->node != node || !e->injected || e->seq == 0) { continue; }
            uint64_t truth = local_tick(m, e->us);
            if (e->seq > r->seq) { assert(truth + 64u > r->tick); }
            else { assert(truth < r->tick + 64u); }
        }
    }
    assert(cps > 0);
}

/* Stamps: skew is applied as a fraction of the measured spans (sync.c), not a rounded
 * ppm, up to SYNC_TTL_MS past its anchor; interpolation is exact to a tick. */
#define TOL_TICKS 128

/* ---- scenarios ------------------------------------------------------------ */
static void random_edges(int node, uint64_t from_us, uint64_t to_us, unsigned mean_gap_ms, uint32_t seed)
{
    uint64_t t = from_us;
    for (;;) {
        seed = mix(seed + 1u);
        t += 300000u + (uint64_t)(seed % (mean_gap_ms * 2000u));
        if (t >= to_us) { break; }
        edge_at(node, t);
        if ((seed >> 8) % 7u == 0u) { /* a bouncing crossing */
            for (unsigned b = 1; b <= 8; b++) { edge_at(node, t + b * 9000u + (seed >> 20) % 3000u); }
        }
    }
}

static unsigned delay_checks;
static int64_t delay_max_error;
static void delay_hook(uint64_t us)
{
    /* Reopen admission while three sensors measure: the converged estimator sees both lengths. */
    if (n_nodes == 4 && ((us >= 80000000u && us < 100000000u) ||
                         (us >= 180000000u && us < 185000000u))) {
        mm.admission.join_until_ms = (uint32_t)(us / 1000u) + 1000u;
        mm.admission.request_seen = 1;
    }
    if (us < 40000000u || us % 100000u) { return; }
    int64_t expected = ((int64_t)up_delay_us - down_delay_us) * 8;
    for (int i = 1; i < n_nodes; i++) {
        const sync_t *s = &nodes[i].s.sync;
        uint64_t local = local_tick(&nodes[i], us);
        assert((sync_health(s, local, 1) & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED);
        int64_t err = (int64_t)(sync_to_master(s, local) - local_tick(&nodes[0], us)) - expected;
        if (err < 0) { err = -err; }
        if (err > delay_max_error) { delay_max_error = err; }
        if (err > 6) {
            fprintf(stderr, "delay model node %d at %llu: error %lld ticks\n", i,
                    (unsigned long long)us, (long long)err);
            abort();
        }
        delay_checks++;
    }
}

static void radio_delay(unsigned count, int asymmetric)
{
    down_delay_us = 144; up_delay_us = asymmetric ? 122 : 144;
    admission_delay_us = 37;
    add_node(1, 0, 0, 0);
    const int32_t ppm[] = {-79, -31, 17, 43, 79};
    for (unsigned i = 0; i < count; i++) { add_node(0, 0x12340001u + i, ppm[i], 0); }
    run(270000000u, delay_hook); /* includes the 8-bit beacon sequence wrap */
    check_slots(0);
    unsigned normal = 0, admission = 0;
    for (unsigned i = 0; i < air_n; i++) if (SEC_VT_TYPE(air[i].data[0]) == PKT_TYPE_UPLINK) {
        if (air[i].len == WIRE_UPLINK) { normal++; } else { admission++; }
    }
    assert(normal && admission && delay_checks > 5000);
    printf("PASS radio_delay_%u_%s (%u checks, max model error %lld ticks)\n", count,
           asymmetric ? "asymmetric" : "symmetric", delay_checks, (long long)delay_max_error);
}

/* Check the actual D lines and their expiry between reports, not just the sensor's clock. */
static int reported_sync_ok(int node, uint64_t us)
{
    assert(timing_report[node].at_us);
    uint64_t elapsed = us > timing_report[node].at_us ? us - timing_report[node].at_us : 0;
    return timing_report[node].valid && timing_report[node].age + elapsed / 1000u <= SYNC_TTL_MS;
}

static unsigned timing_checks;
static int timing_failure;
static void timing_hook(uint64_t us)
{
    if (timing_failure) {
        if (us == 40000000u) { nodes[1].deaf = 1; }
        if (us == 55000000u) { nodes[1].deaf = 0; }
        if (us >= 48000000u && us < 55000000u && us % 100000u == 0) {
            assert(!(sync_health(&nodes[1].s.sync, local_tick(&nodes[1], us), 1) & HEALTH_SYNC_VALID));
            assert(!reported_sync_ok(1, us));
        }
        if (us >= 40000000u && us < 80000000u) { return; }
    } else if (n_nodes == 4 && us >= 80000000u && us < 100000000u) {
        mm.admission.join_until_ms = (uint32_t)(us / 1000u) + 1000u;
        mm.admission.request_seen = 1;
    }
    if (us < 30000000u || us % 100000u) { return; }
    for (int i = 1; i < n_nodes; i++) {
        assert(sync_health(&nodes[i].s.sync, local_tick(&nodes[i], us), 1) == HEALTH_EVENT_REQUIRED);
        if (!reported_sync_ok(i, us)) {
            fprintf(stderr, "false sync expiry node %d at %llu: reported age %u, received at %llu\n",
                    i, (unsigned long long)us, timing_report[i].age, (unsigned long long)timing_report[i].at_us);
            abort();
        }
        timing_checks++;
    }
}

static void sync_reports(unsigned count, int failure)
{
    track_timing = 1; timing_failure = failure;
    down_delay_us = up_delay_us = 144; admission_delay_us = 37;
    add_node(1, 0, 0, 0);
    for (unsigned i = 0; i < count; i++) { add_node(0, 0x12340001u + i, (int32_t)i * 31 - 62, 0); }
    run(failure ? 100000000u : 270000000u, timing_hook);
    check_slots(0);
    assert(timing_checks > 200);
    if (!failure) {
        /* No echo loop: one checkpoint and one status per idle node per 5 s, five nodes too. */
        for (int n = 1; n < n_nodes; n++) {
            unsigned checkpoints = 0, reports = 0;
            for (unsigned i = 0; i < air_n; i++) {
                const tx_t *x = &air[i];
                if (x->src != n || x->start_us < 110000000u || x->start_us >= 210000000u) { continue; }
                assert(SEC_VT_TYPE(x->data[0]) == PKT_TYPE_UPLINK);
                const uplink_pl_t *p = (const uplink_pl_t *)x->plain;
                if (p->kind == UL_KIND_CHECKPOINT) { checkpoints++; }
                else { assert(p->kind == UL_KIND_SYNC); reports++; }
            }
            if (checkpoints != 20 || reports != 20) {
                fprintf(stderr, "idle node %d: %u checkpoints, %u reports / 100 s\n", n, checkpoints, reports);
                abort();
            }
        }
    }
    printf("PASS sync_reports_%u%s (%u status checks)\n", count, failure ? "_loss" : "", timing_checks);
}

static void sync_reports_data(void)
{
    track_timing = 1;
    add_node(1, 0, 0, 0); add_node(0, 0x12340001u, -18, 0);
    for (uint64_t us = 29001000u; us < 80000000u; us += 1000000u) { edge_at(1, us); }
    run(85000000u, timing_hook);
    check_slots(0);
    unsigned interp, lost, packets = 0;
    check_records(1, 29000000u, TOL_TICKS, &interp, &lost);
    assert(!lost);
    for (unsigned i = 0; i < air_n; i++) {
        const tx_t *x = &air[i];
        if (x->src != 1 || x->start_us < 30000000u || x->start_us >= 80000000u) { continue; }
        const uplink_pl_t *p = (const uplink_pl_t *)x->plain;
        assert(SEC_VT_TYPE(x->data[0]) == PKT_TYPE_UPLINK && p->kind == UL_KIND_EDGES);
        packets++;
    }
    assert(packets == 50); /* every update rides evidence; no extra status packet */
    puts("PASS sync_reports_piggyback_on_evidence");
}

static int report_rejected, report_kept, report_retried;
static unsigned report_air;
static void report_retry_hook(uint64_t us)
{
    mac_sensor_t *s = &nodes[1].s;
    if (us >= 30000000u && !report_rejected && s->sync_report_pending && !s->tx_busy) {
        nodes[1].reject_tx_once = 1;
        report_air = air_n;
        report_rejected = 1;
    }
    if (report_rejected && !nodes[1].reject_tx_once && !report_kept) {
        assert(s->sync_report_pending && !s->tx_busy && air_n == report_air);
        report_kept = 1;
    }
    if (report_kept && !s->sync_report_pending && !report_retried) {
        assert(air_n > report_air);
        const tx_t *x = &air[air_n - 1];
        const uplink_pl_t *p = (const uplink_pl_t *)x->plain;
        assert(x->src == 1 && p->kind == UL_KIND_SYNC);
        report_retried = 1;
    }
    timing_hook(us);
}

static void sync_reports_retry(void)
{
    track_timing = 1;
    add_node(1, 0, 0, 0); add_node(0, 0x12340001u, -18, 0);
    run(45000000u, report_retry_hook);
    assert(report_rejected && report_kept && report_retried);
    check_slots(0);
    puts("PASS sync_reports_retry_after_tx_start_failure");
}

static void steady(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 23, 2);
    add_node(0, 0x7A1C9F02u, -31, 2);
    add_node(0, 0x11110003u, 7, 2);
    for (int s = 1; s <= 3; s++) { random_edges(s, 12000000u, 110000000u, 2500, (uint32_t)s * 77u); }
    run(125000000u, NULL);
    check_slots(15000000u);
    for (int s = 1; s <= 3; s++) {
        unsigned interp, lost;
        check_records(s, 12000000u, TOL_TICKS, &interp, &lost);
        assert(lost == 0);
        check_checkpoints(s);
    }
    assert(d_lines > 0);
    printf("PASS steady (%u records, %u transmissions)\n", n_recs, air_n);
}

/* Two sensors, captures 1.5..3 s apart, each in its next dealt slot (0, 2, 4 / 1, 3; 5 joins):
 * waits of at most ~520 / ~760 ms, ~255 ms on average, plus ~90 ms of CAD, TX setup, airtime. */
static void latency(int lbt)
{
    lbt_on = lbt;
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 17, 0);
    add_node(0, 0x7A1C9F02u, -12, 0);
    uint32_t seed = 41u;
    for (int s = 1; s <= 2; s++) {
        uint64_t t = 12000000u + (uint64_t)s * 333000u;
        for (;;) {
            seed = mix(seed + 1u);
            t += 1500000u + seed % 1500000u;
            if (t >= 110000000u) { break; }
            edge_at(s, t);
        }
    }
    run(125000000u, NULL);
    check_slots(15000000u);
    uint64_t sum = 0, worst = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < n_edges; i++) {
        const edge_t *e = &edges[i];
        for (unsigned j = 0; j < n_recs; j++) {
            const rec_t *r = &recs[j];
            if (r->kind != 'C' || r->node != nodes[e->node].sender_id || r->seq != e->seq) { continue; }
            uint64_t d = r->at_us - e->us;
            sum += d;
            n++;
            if (d > worst) { worst = d; }
        }
    }
    assert(n == n_edges);
    printf("latency (LBT %s): mean %llu ms, worst %llu ms over %u captures\n", lbt ? "on" : "off",
           (unsigned long long)(sum / n / 1000u), (unsigned long long)(worst / 1000u), n);
    assert(sum / n < 400000u && worst < 1000000u);
    printf("PASS latency%s\n", lbt ? "" : "_lbt_off");
}

static void stall_hook(uint64_t us)
{
    if (us == 20000000u) { host_acks = 0; }
    if (us == 70000000u) { host_acks = 1; }
}

static void overflow(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 11, 0);
    add_node(0, 0x7A1C9F02u, -5, 0);
    /* the host stops acking: the master queue fills, then sensor 1's FIFO */
    for (unsigned i = 0; i < 400; i++) { edge_at(1, 25000000u + (uint64_t)i * 90000u); }
    random_edges(2, 15000000u, 90000000u, 3000, 5u);
    run(160000000u, stall_hook);
    unsigned interp, lost, lost2;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost > 0);                  /* the FIFO overflowed into a loss range ... */
    assert(el_count(EL_FIFO_FULL) > 0);
    check_records(2, 12000000u, TOL_TICKS, &interp, &lost2);
    assert(lost2 == 0);                /* ... without touching the other sensor */
    printf("PASS overflow (%u captures kept as a loss range)\n", lost);
}

static void deaf_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[1].deaf = 1; }
    if (us == 50000000u) { nodes[1].deaf = 0; }
}

static void sync_gap(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 38, 0);
    add_node(0, 0x7A1C9F02u, -22, 0);
    edge_at(1, 20000000u);
    edge_at(1, 39500000u);  /* 9.5 s after the last anchor: held, then interpolated */
    edge_at(1, 45000000u);
    edge_at(1, 60000000u);
    edge_at(2, 40000000u);  /* the other sensor is unaffected */
    run(80000000u, deaf_hook);
    unsigned interp, lost;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(interp == 2 && lost == 0);
    check_records(2, 12000000u, TOL_TICKS, &interp, &lost);
    assert(interp == 0);
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].node == nodes[2].sender_id && recs[j].kind == 'C') {
            assert(recs[j].at_us < 40000000u + 2500000u); /* delivered with no extra delay */
        }
    }
    printf("PASS sync_gap\n");
}

static void long_deaf_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[1].deaf = 1; }
    if (us == 100000000u) { nodes[1].deaf = 0; }
}

static void long_gap(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 38, 0);
    edge_at(1, 20000000u);
    edge_at(1, 50000000u);  /* anchors 70 s apart cannot bracket it: a loss */
    edge_at(1, 120000000u);
    run(140000000u, long_deaf_hook);
    unsigned interp, lost;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 1 && interp == 0);
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].kind == 'L') { assert(recs[j].flags & EVENT_TIME_UNKNOWN); }
    }
    assert(nodes[1].begin_calls >= 1); /* no beacon for 10 s: the radio was reset */
    printf("PASS long_gap\n");
}

/* Sensor 1 reboots and rejoins via JOIN/OFFER/CONFIRM into its own entry, even with a full
 * table. Same/new-tag runs differ only in the new boot ID's low byte; both rejoin alike. */
static int reboot_same_tag;

static void reboot_hook(uint64_t us)
{
    if (us == 40000000u) {
        node_t *n = &nodes[1];
        cur = n;
        n->boot_id = reboot_same_tag ? (n->boot_id & 0xFFu) | 0x5A5A0000u
                                     : ((n->boot_id + 1u) & 0xFFu) | 0x5A5A0000u;
        n->tx_ctr = 0;
        n->cseq = 0;
        n->rh = n->rt = 0;
        mac_sensor_init(&n->s, n->sender_id, 0);
    }
}

static void reboot(int same_tag, int sensors)
{
    static const uint32_t ids[] = { 0x0D2243B0u, 0x7A1C9F02u, 0x11110003u, 0x22220004u, 0x33330005u };
    reboot_same_tag = same_tag;
    add_node(1, 0, 0, 0);
    for (int i = 0; i < sensors; i++) { add_node(0, ids[i], (i % 2 ? 29 : -17) + i, 1); }
    random_edges(1, 12000000u, 38000000u, 2000, 3u);
    random_edges(1, 52000000u, 90000000u, 2000, 4u);
    for (int i = 2; i <= sensors; i++) { random_edges(i, 12000000u, 90000000u, 2000, 9u * (uint32_t)i); }
    run(110000000u, reboot_hook);
    check_slots(15000000u);
    unsigned interp, lost;
    for (int i = 2; i <= sensors; i++) { check_records(i, 12000000u, TOL_TICKS, &interp, &lost); }
    /* after the reboot every capture of the new boot arrives */
    uint64_t new_boot = nodes[1].boot_id;
    unsigned got = 0, want = 0;
    for (unsigned i = 0; i < n_edges; i++) { if (edges[i].node == 1 && edges[i].us >= 52000000u) { want++; } }
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].node == nodes[1].sender_id && recs[j].sboot == new_boot && recs[j].kind == 'C') { got++; }
        if (recs[j].node == nodes[1].sender_id && recs[j].sboot == new_boot) { assert(recs[j].kind != 'L'); }
    }
    assert(got == want && want > 0);
    printf("PASS reboot (%s tag, %d sensors)\n", same_tag ? "same" : "new", sensors);
}

/* ev_seq wraps (it skips 0): the master takes it as the next packet, no gap. */
static void ev_wrap(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 21, 0);
    nodes[1].s.q.next_ev_seq = 0xFFF0u;
    random_edges(1, 12000000u, 80000000u, 1000, 17u);
    run(90000000u, NULL);
    assert(nodes[1].s.q.next_ev_seq < 0xFFF0u); /* it wrapped */
    assert(el_count(EL_ACK_GAP) == 0);
    unsigned interp, lost;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 0);
    printf("PASS ev_wrap\n");
}

static void hfxo_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[1].hfxo_stops++; } /* restarted before the MAC looked */
}

/* A masked HFXO stop drops sync: captures are unknown-time until fresh anchors and skew. */
static void hfxo_stop(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 26, 0);
    edge_at(1, 20000000u);
    edge_at(1, 30300000u);
    edge_at(1, 45000000u);
    run(60000000u, hfxo_hook);
    unsigned interp, lost;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 1);
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].kind == 'L') { assert(recs[j].flags & EVENT_TIME_UNKNOWN && recs[j].seq == 2); }
    }
    printf("PASS hfxo_stop\n");
}

static void xtal_lost_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[1].xtal = 0; } /* the crystal stays down */
}

/* A crystal-less sensor discards every anchor (always reports the oldest) yet must not take
 * every frame's two-way reply: the others keep fresh anchors and all their captures stamped. */
static void xtal_lost(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 26, 0);
    add_node(0, 0x7A1C9F02u, -22, 0);
    add_node(0, 0x11110003u, 31, 0);
    for (int i = 2; i <= 3; i++) { random_edges(i, 40000000u, 140000000u, 2000, 5u * (uint32_t)i); }
    run(150000000u, xtal_lost_hook);
    const reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
    assert(e);
    unsigned rc_entry = (unsigned)(e - mm.reg.e), replies = 0;
    for (unsigned i = 0; i < air_n; i++) {
        if (!is_beacon(&air[i]) || air[i].start_us < 32000000u) { continue; }
        const beacon_pl_t *b = (const beacon_pl_t *)air[i].plain;
        if (b->flags & BEACON_RX_PREV_VALID) { assert(b->rx_entry != rc_entry); replies++; }
    }
    assert(replies > 0);
    unsigned interp, lost;
    for (int i = 2; i <= 3; i++) {
        check_records(i, 40000000u, TOL_TICKS, &interp, &lost);
        assert(interp == 0 && lost == 0);
    }
    printf("PASS xtal_lost\n");
}

static void faults_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[2].noresp = (int)RADIO_NORESP_RESET; }
    if (us == 35000000u) { assert(nodes[2].begin_calls == 1); }
    if (us == 45000000u) {
        /* master timebase session change (clock fault recovery path) */
        nodes[1].s.sync.rx_miss = 321;
        cur = &nodes[0];
        (void)mq_push_timebase_end(&mq, nodes[0].boot_id, capture_now64());
        nodes[0].boot_id = mix(nodes[0].boot_id);
        nodes[0].tx_ctr = 0;
        mac_master_session(&mm);
    }
}

static void faults(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 12, 0);
    add_node(0, 0x7A1C9F02u, -3, 0);
    edge_at(1, 70000000u);
    edge_at(2, 71000000u);
    run(80000000u, faults_hook);
    assert(nodes[1].s.sync.rx_miss >= 321); /* lifetime total survives master session replacement */
    assert(nodes[2].begin_calls >= 1); /* the SPI fault recovered before the new-master handshake */
    int saw_end = 0;
    unsigned after = 0;
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].node == 0) { saw_end = 1; assert(recs[j].kind == 'L'); }
        if (recs[j].kind == 'C') { assert(recs[j].mboot == nodes[0].boot_id); after++; }
    }
    assert(saw_end && after == 2);
    printf("PASS faults\n");
}

/* Authenticated wrong-session packets must not take a registry entry or advance a replay window. */
static void inject_uplink(uplink_pl_t *u)
{
    uint8_t wire[WIRE_MAX];
    cur = &nodes[1];
    int n = sec_seal_key(cur->s.data_key, wire, sizeof wire, PKT_TYPE_UPLINK, cur->sender_id, u, sizeof *u);
    cur = &nodes[0];
    memcpy(cur->pkt, wire, (size_t)n);
    cur->pkt_len = n; cur->have_pkt = 1;
    mm.next_beacon = UINT64_MAX;
    mac_master_step(&mm);
}

/* Recorded ciphertext straight to a receiver, skipping RF collisions; real MAC and Monocypher. */
static void replay_wire(int receiver, const uint8_t *wire, int len)
{
    cur = &nodes[receiver]; cur->now_us = now_global;
    assert(!cur->transmitting);
    memcpy(cur->pkt, wire, (size_t)len); cur->pkt_len = len; cur->have_pkt = 1;
    cur->dio1 = 1; cur->dio1_tick = capture_now64();
    if (receiver) { cur->s.tx_slots = 0; mac_sensor_step(&cur->s); }
    else {
        uint64_t next = mm.next_beacon; mm.next_beacon = UINT64_MAX;
        mac_master_step(&mm); mm.next_beacon = next;
    }
}
static tx_t recorded(unsigned type)
{
    for (unsigned i = 0; i < air_n; i++) if (SEC_VT_TYPE(air[i].data[0]) == type) return air[i];
    fprintf(stderr, "No packet type %u\n", type); abort();
}
static void roundtrip_binding(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    run(15900000u, NULL);
    reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
    assert(e && nodes[1].s.timing_ready);
    uint16_t ack = e->ack_seq; int have_ack = e->have_ack;
    unsigned depth = mq_depth(&mq);
    unsigned entry = (unsigned)(e - mm.reg.e);
    uint8_t wire[WIRE_MAX];
    uplink_pl_t u = {.kind = UL_KIND_SYNC, .flags = EVENT_TIME_UNKNOWN,
                     .master_boot_id = nodes[0].boot_id, .ev_seq = 500,
                     .diag.health = HEALTH_CLOCK_XTAL};
    memset(mm.schedule, 0, sizeof(mm.schedule)); mm.schedule[0] = SLOT_ROLE_DATA | entry;
    mm.cycle_ready = 1; mm.cycle_wire_len = WIRE_UPLINK; mm.reply_valid = 0;
    mm.cycle_tx = local_tick(&nodes[0], now_global) - (SLOT_OFFSET_MS + 50u) * TICKS_PER_MS;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        u.beacon_ctr = mm.beacon_ctr + (attempt == 0 ? 1u : 0u);
        cur = &nodes[1];
        int n = sec_seal_uplink(cur->s.data_key, wire, sizeof(wire), cur->sender_id,
                               &u, sizeof(u), attempt == 1 ? WIRE_UPLINK_ADMISSION : WIRE_UPLINK);
        assert(n > 0);
        if (attempt == 2) { mm.cycle_ready = 0; }
        if (attempt == 3) { mm.cycle_ready = 1; }
        replay_wire(0, wire, n);
        assert(!!mm.reply_valid == (attempt == 3));
        assert(e->ack_seq == ack && e->have_ack == have_ack && mq_depth(&mq) == depth);
    }
    assert(mm.reply_entry == entry && mm.reply_link == e->link_id);
    uint64_t reply = mm.reply_rx;
    replay_wire(0, wire, WIRE_UPLINK); /* replay cannot change the selected capture */
    assert(mm.reply_rx == reply);
    puts("PASS roundtrip_frame_length_counter_and_probe_ack_binding");
}
static void session_replay(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    uplink_pl_t u = {.kind = UL_KIND_EDGES, .flags = HEALTH_EVENT_REQUIRED, .ev_seq = 1, .capture_seq = 1};
    u.u.edges.count = 1; u.master_boot_id = nodes[0].boot_id;
    uint64_t next = mm.next_beacon;
    inject_uplink(&u); /* authenticated with no negotiated key: no auto-registration */
    assert(reg_count(&mm.reg) == 0 && mq_depth(&mq) == 0); mm.next_beacon = next;
    run(15900000u, NULL);
    assert(nodes[1].s.have_session && nodes[1].s.timing_ready && reg_count(&mm.reg) == 1);
    tx_t beacon = recorded(PKT_TYPE_BEACON), request = recorded(PKT_TYPE_JOIN);
    tx_t offer = recorded(PKT_TYPE_OFFER), confirm = recorded(PKT_TYPE_CONFIRM), data = recorded(PKT_TYPE_UPLINK);
    reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
    reg_entry_t saved = *e;
    sensor_queue_t q = nodes[1].s.q; sync_t sync = nodes[1].s.sync;
    uint32_t high = nodes[1].s.from_master.max_ctr;
    uint64_t link = nodes[1].s.link_id;
    replay_wire(0, data.data, data.len);
    assert(e->rx.max_ctr == saved.rx.max_ctr && e->ack_seq == saved.ack_seq && mq_depth(&mq) == 0);
    replay_wire(0, request.data, request.len); replay_wire(0, confirm.data, confirm.len);
    assert(mm.admission.pending[0].phase == ADMISSION_EMPTY && e->link_id == link);
    replay_wire(1, beacon.data, beacon.len); replay_wire(1, offer.data, offer.len);
    assert(nodes[1].s.from_master.max_ctr == high && nodes[1].s.link_id == link);
    assert(!memcmp(&nodes[1].s.q, &q, sizeof(q)) && !memcmp(&nodes[1].s.sync, &sync, sizeof(sync)));
    /* An authentic but never-seen old master epoch cannot retire the live
     * epoch, apply ACK/CP, reset sync, or erase queued evidence. */
    uint8_t wire[WIRE_MAX]; beacon_pl_t old = { .cp_req = 77 };
    cur = &nodes[0]; uint64_t boot = cur->boot_id; cur->boot_id ^= UINT64_C(1) << 40;
    int n = sec_seal(wire, sizeof(wire), PKT_TYPE_BEACON, 0, &old, sizeof(old)); cur->boot_id = boot;
    replay_wire(1, wire, n);
    assert(nodes[1].s.master_boot_id == boot && nodes[1].s.link_id == link && nodes[1].s.join.phase == SENSOR_JOIN_IDLE);
    /* Even after losing the live master, a beacon is merely an untrusted hint. */
    nodes[1].s.last_beacon_ms -= SYNC_TTL_MS + 1;
    replay_wire(1, wire, n);
    assert(nodes[1].s.join.phase == SENSOR_JOIN_WAIT_OFFER);
    assert(nodes[1].s.master_boot_id == boot && nodes[1].s.link_id == link);
    assert(!memcmp(&nodes[1].s.q, &q, sizeof(q)) && !memcmp(&nodes[1].s.sync, &sync, sizeof(sync)));
    memset(&nodes[1].s.join, 0, sizeof(nodes[1].s.join));
    /* Evict all request history and active replay state: an old JOIN may
     * reserve work, but its old CONFIRM must fail with fresh Nm and link ID. */
    cur = &nodes[0]; memset(e, 0, sizeof(*e)); /* discard state in the test fixture */
    mm.admission.seen_count = 0;
    replay_wire(0, request.data, request.len);
    assert(mm.admission.pending[0].phase != ADMISSION_EMPTY && mm.admission.pending[0].link_id > link);
    master_admission_offer_started(&mm.admission, 0, 1);
    replay_wire(0, confirm.data, confirm.len); replay_wire(0, data.data, data.len);
    assert(reg_count(&mm.reg) == 0 && mq_depth(&mq) == 0);
    /* Same epoch, fresh connection: retained FIFO identity survives. */
    memset(mm.admission.pending, 0, sizeof(mm.admission.pending));
    sq_push_edge(&nodes[1].s.q, 100, local_tick(&nodes[0], now_global), HEALTH_EVENT_REQUIRED, 1);
    run(35900000u, NULL);
    assert(nodes[1].s.have_session && nodes[1].s.link_id > link && nodes[1].s.master_boot_id == boot);
    e = reg_lookup(&mm.reg, nodes[1].sender_id); assert(e && e->link_id == nodes[1].s.link_id);
    saved = *e;
    replay_wire(0, confirm.data, confirm.len); replay_wire(0, data.data, data.len);
    assert(e->link_id == saved.link_id && e->rx.max_ctr == saved.rx.max_ctr && e->ack_seq == saved.ack_seq);
    unsigned copies = 0;
    for (unsigned i = 0; i < n_recs; i++) if (recs[i].node == e->id && recs[i].kind == 'C' && recs[i].seq == 100) copies++;
    assert(copies == 1);
    puts("PASS session_replay (all five packet types, unknown epoch, eviction, fresh keys, retained evidence)");
}

static void late_hook(uint64_t us)
{
    if (us == 20000000u) { seal_delay_us = 10000; }
    if (us == 25000000u) { seal_delay_us = 0; }
}

static void late_seal(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x12345678u, 0, 0);
    random_edges(1, 12000000u, 40000000u, 500, 17u);
    run(70000000u, late_hook);
    check_slots(12000000u);
    for (unsigned i = 0; i < air_n; i++) {
        if (air[i].src == 1) { assert(air[i].start_us < 20000000u || air[i].start_us >= 25000000u); }
    }
    unsigned interp, lost;
    check_records(1, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 0);
    puts("PASS late_seal");
}

static uint32_t late_slot_delay_us;
static void late_slot_hook(uint64_t us)
{
    for (int i = 0; i < n_nodes; i++) {
        node_t *n = &nodes[i];
        if (n->now_us > us) { continue; }
        /* Delay OFFERs during admission, then DATA after the sensor joins. */
        if (n->is_master ? us >= 5000000u : us < 20000000u || us >= 25000000u) { continue; }
        for (unsigned j = 0; j < SLOT_COUNT; j++) {
            if (n->is_master) {
                if (!mm.cycle_ready || (mm.slot_done & (1u << j)) ||
                    SLOT_ROLE(mm.schedule[j]) != SLOT_ROLE_OFFER) { continue; }
            } else if (!(n->s.tx_slots & (1u << j))) { continue; }
            uint64_t tick = (n->is_master ? mm.cycle_tx : n->s.cycle_rx) +
                            (SLOT_OFFSET_MS + j * SLOT_LEN_MS) * TICKS_PER_MS;
            uint64_t start = (tick - n->tick_base) / 16u; /* this scenario has zero drift */
            if (us + LBT_LISTEN_MS * 1000u >= start && us < start + late_slot_delay_us) {
                n->now_us = start + late_slot_delay_us;
            }
            break;
        }
    }
}

static void late_slot(uint32_t delay_us)
{
    late_slot_delay_us = delay_us;
    add_node(1, 0, 0, 0); add_node(0, 0x12345678u, 0, 0);
    random_edges(1, 18000000u, 40000000u, 500, 17u);
    run(70000000u, late_slot_hook);
    if (delay_us > LBT_LISTEN_MS * 1000u) {
        assert(nodes[0].lbt_skips && nodes[1].lbt_skips);
    } else {
        assert(nodes[0].cad_skips && nodes[1].cad_skips);
    }
    assert(nodes[0].begin_calls == 0 && nodes[1].begin_calls == 0);
    assert(nodes[1].s.have_session);
    check_slots(0);
    unsigned interp, lost;
    check_records(1, 18000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 0);
    printf("PASS late_slot_%s (OFFER and DATA cancel, remain connected, retain captures)\n",
           delay_us > LBT_LISTEN_MS * 1000u ? "lbt" : "cad");
}

/* Jam master 20-23 s, then sensor 1 40-43 s: the jammed node stays silent; all captures arrive. */
static void lbt_jam(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 19, 0);
    add_node(0, 0x7A1C9F02u, -8, 0);
    nodes[0].jam_from_us = 20000000u; nodes[0].jam_to_us = 23000000u;
    nodes[1].jam_from_us = 40000000u; nodes[1].jam_to_us = 43000000u;
    random_edges(1, 12000000u, 60000000u, 700, 3u);
    random_edges(2, 12000000u, 60000000u, 700, 9u);
    run(80000000u, NULL);
    check_slots(12000000u);
    unsigned beacons_after = 0, sensor_after = 0;
    for (unsigned i = 0; i < air_n; i++) {
        const tx_t *x = &air[i];
        /* A listen that ended before the jam may still be followed by a CAD and the TX setup. */
        for (int n = 0; n < 2; n++) {
            if (x->src == n) {
                assert(x->start_us < nodes[n].jam_from_us + 20000u || x->start_us > nodes[n].jam_to_us + TX_SETUP_US);
            }
        }
        if (x->src == 0 && x->start_us > 23000000u && x->start_us < 25000000u) { beacons_after++; }
        if (x->src == 1 && x->start_us > 43000000u && x->start_us < 46000000u) { sensor_after++; }
    }
    assert(beacons_after > 0 && sensor_after > 0);
    unsigned interp, lost;
    for (int n = 1; n <= 2; n++) {
        check_records(n, 12000000u, TOL_TICKS, &interp, &lost);
        assert(lost == 0);
    }
    puts("PASS lbt_jam");
}

static void cad_failure(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x12345678u, 0, 0);
    cad_error = 1;
    run(5000000u, NULL);
    assert(air_n == 0 && !mm.m_tx_valid);
    puts("PASS cad_failure_no_transmit");
}

static unsigned peak_fifo;
static void load_hook(uint64_t us)
{
    if (us == 50000000u) { host_acks = 0; }
    if (us == 65000000u) { host_acks = 1; }
    for (int i = 1; i < n_nodes; i++) {
        if (nodes[i].s.q.count > peak_fifo) { peak_fifo = nodes[i].s.q.count; }
    }
}

static void load(unsigned sensors, unsigned rate)
{
    add_node(1, 0, 0, 10); /* inject loss on uplinks as well as sensor beacon reception */
    for (unsigned i = 1; i <= sensors; i++) {
        add_node(0, 0x12340000u + i, (int)i * 5, 10);
        for (uint64_t us = 32000000u + i * 20000u; us < 72000000u; us += 1000000u / rate) { edge_at((int)i, us); }
    }
    run(180000000u, load_hook);
    check_slots(20000000u);
    unsigned total_lost = 0;
    for (unsigned i = 1; i <= sensors; i++) {
        unsigned interp, lost;
        check_records((int)i, 12000000u, TOL_TICKS, &interp, &lost);
        total_lost += lost;
        const sensor_queue_t *q = &nodes[i].s.q;
        assert(q->count == 0 && !q->acc_pending);
        /* Periodic checkpoints can still be awaiting their next beacon ACK. */
        assert(!q->inflight || q->pkt.kind == UL_KIND_CHECKPOINT);
    }
    if (rate == 3) { assert(total_lost == 0 && peak_fifo < SENSOR_FIFO_LEN); }
    else { assert(total_lost > 0 && peak_fifo == SENSOR_FIFO_LEN); }
    printf("PASS load (%u sensors, %u Hz, 10%% RX loss, 15s PC stall): peak FIFO %u, lost %u\n",
           sensors, rate, peak_fifo, total_lost);
}

static void recovery_step(void)
{
    if (cur->is_master) { (void)mac_master_step(&mm); }
    else { mac_sensor_step(&cur->s); }
}

/* Failed config and RX startup back off, sparing timebase, session and queued evidence;
 * no ordinary radio work clears them. */
static void init_recovery(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x12345678u, 0, 0);
    assert(mq_push_timebase_end(&mq, nodes[0].boot_id, 1234));
    mq_item_t queued = mq.item[mq.head];
    sq_push_edge(&nodes[1].s.q, 1, 5678, HEALTH_EVENT_REQUIRED, 0);
    for (int i = 0; i < n_nodes; i++) {
        cur = &nodes[i];
        uint64_t boot = cur->boot_id, base = cur->tick_base;
        retry_t *retry = i == 0 ? &mm.retry : &cur->s.retry;
        retry_init(retry, board_millis(), 100u);
        cur->init_pending = cur->begin_fail = 1;
        cur->s.tx_slots = 1;
        recovery_step();
        assert(cur->begin_calls == 0 && cur->receive_calls == 0 && air_n == 0);
        cur->now_us = (uint64_t)retry->at_ms * 1000u;
        recovery_step();
        assert(cur->begin_calls == 1 && radio_needs_reset());
        cur->begin_fail = 0; cur->rx_fail = 1;
        cur->now_us = (uint64_t)retry->at_ms * 1000u;
        recovery_step();
        assert(cur->begin_calls == 2 && radio_needs_reset());
        cur->now_us = (uint64_t)(retry->at_ms - 1u) * 1000u;
        recovery_step();
        assert(cur->begin_calls == 2 && cur->receive_calls == 0 && air_n == 0);
        cur->rx_fail = 0;
        cur->now_us = (uint64_t)retry->at_ms * 1000u;
        recovery_step();
        assert(cur->begin_calls == 3 && !radio_needs_reset() && cur->rx_on);
        assert(cur->boot_id == boot && cur->tick_base == base);
        assert(mq_depth(&mq) == 1 && memcmp(&mq.item[mq.head], &queued, sizeof(queued)) == 0);
        assert(nodes[1].s.q.count == 1 && nodes[1].s.q.item[0].tick == 5678);
        if (i == 1) { assert(cur->s.tx_slots == 0); }
    }
    cur = &nodes[0];
    mm.next_beacon = capture_now64();
    recovery_step();
    assert(air_n == 1 && cur->receive_calls > 0);
    puts("PASS init_recovery");
}

/* A registered sensor as admission leaves it (master_admission.c). */
static reg_entry_t *sim_entry(unsigned slot, uint32_t id, uint32_t heard_ms)
{
    reg_entry_t *e = &mm.reg.e[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1; e->id = id; e->heard_ms = heard_ms; e->last_state = -1;
    return e;
}

static void status_reports(void)
{
    add_node(1, NODE_MASTER, 0, 0);
    reg_entry_t *e = sim_entry(0, 1, 0);
    e->last_state = PU_STATE_OK;
    reg_entry_t *other = sim_entry(1, 2, 0);
    other->last_state = PU_STATE_OK;
    unsigned previous = d_lines;
    /* Both sensors go silent at t=0: ages keep rising, transitions are immediate, LOST repeats. */
    for (unsigned ms = 0; ms <= 40000; ms += 1000) {
        cur->now_us = (uint64_t)ms * 1000u;
        mac_master_report(&mm, 1);
        int expected = (ms && ms % SENSOR_STATUS_PERIOD_MS == 0) || ms == 13000 || ms == 18000;
        assert(d_lines - previous == (expected ? 2u : 0u));
        if (expected) {
            unsigned id, age;
            char state[16];
            assert(sscanf(last_d, "D %x %15s %*s %*s %*s %u", &id, state, &age) == 3);
            assert(id == 2 && age == ms);
            assert(!strcmp(state, ms <= 12000 ? "OK" : ms <= 17000 ? "STALE" : "LOST"));
        }
        assert(e->heard_ms == 0 && other->heard_ms == 0);
        previous = d_lines;
    }
    /* Explicit ?STATUS still returns all entries immediately. */
    mac_master_report(&mm, 0);
    assert(d_lines == previous + 2);

    /* Periodic refresh also works across board_millis wrap. */
    cur->now_us = (uint64_t)(UINT32_MAX - 2000u) * 1000u;
    mac_master_init(&mm, &mq);
    e = sim_entry(0, 3, board_millis());
    e->last_state = PU_STATE_OK;
    previous = d_lines;
    cur->now_us += (SENSOR_STATUS_PERIOD_MS - 1u) * 1000u;
    mac_master_report(&mm, 1);
    assert(d_lines == previous);
    cur->now_us += 1000u;
    mac_master_report(&mm, 1);
    assert(d_lines == previous + 1);
    puts("PASS status_reports");
}

static int admission_case;
static uint64_t first_join[SIM_NODES], first_ready[SIM_NODES], old_link, incumbent_heard;
static int race_guard;
static unsigned duplicate_types;
static void admission_hook(uint64_t us)
{
    for (int i = 1; i < n_nodes; i++) {
        if (nodes[i].s.have_session && !first_join[i]) first_join[i] = us;
        if (nodes[i].s.timing_ready && !first_ready[i]) first_ready[i] = us;
    }
    if (admission_case == 1 && us == 20000000u) { /* four active, add the fifth */
        add_node(0, 0x10000005u, 8, 0);
    }
    if (admission_case == 2 && us == 20000000u) { /* five active, same-epoch reauth */
        old_link = nodes[1].s.link_id; assert(old_link);
        nodes[1].s.link_id = 0;
        memset(nodes[1].s.data_key, 0, 32);
        sq_push_edge(&nodes[1].s.q, 100, local_tick(&nodes[0], us), HEALTH_EVENT_REQUIRED, 1);
    }
    if ((admission_case == 3 || admission_case == 4 || admission_case == 5) && us == 20000000u) {
        nodes[1].disabled = admission_case != 4; /* a sixth sensor cannot evict a live fleet */
        cur = &nodes[0]; reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id); assert(e);
        incumbent_heard = e->heard_ms;
        if (admission_case == 5) e->heard_ms = 1; /* eligible for a LOST reservation */
        add_node(0, 0x20000006u, 8, 0);
    }
    if (admission_case == 5 && n_nodes == 7 && !race_guard) {
        for (unsigned i = 0; i < MAX_NODES; i++) {
            if (mm.admission.pending[i].phase != ADMISSION_EMPTY && mm.admission.pending[i].id == nodes[6].sender_id) {
                assert(mm.reg.e[i].id == nodes[1].sender_id);
                mm.reg.e[i].heard_ms = (uint32_t)(us / 1000u); race_guard = 1;
            }
        }
    }
    if (admission_case >= 6 && admission_case <= 8 && us == 10000000u) nodes[admission_case == 7 ? 1 : 0].drop_type = 0;
    if (admission_case == 9) { /* lose the registration-complete beacon(s) */
        if (!race_guard && reg_count(&mm.reg)) { nodes[1].deaf = 1; race_guard = 1; }
        if (us == 8000000u) nodes[1].deaf = 0;
    }
    if (admission_case == 10 && us == 1200000u) mm.admission.join_until_ms = 1200; /* cancel an active exchange */
    if (admission_case == 11) {
        for (unsigned a = 0; a < air_n; a++) {
            const tx_t *x = &air[a]; unsigned type = SEC_VT_TYPE(x->data[0]);
            if (duplicate_types & (1u << type) || x->end_us > us) continue;
            int receiver = type == PKT_TYPE_BEACON || type == PKT_TYPE_OFFER ? 1 : 0;
            if (nodes[receiver].transmitting || !((x->seen >> receiver) & 1u)) continue;
            mac_sensor_t *sensor = &nodes[1].s;
            reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
            if (type == PKT_TYPE_BEACON && (!sensor->hint_valid || sensor->hint_ctr != sec_wire_counter(x->data))) continue;
            if (type == PKT_TYPE_JOIN && mm.admission.pending[0].phase == ADMISSION_EMPTY) continue;
            if (type == PKT_TYPE_OFFER &&
                ((sensor->join.phase != SENSOR_JOIN_CONFIRM_READY && sensor->join.phase != SENSOR_JOIN_WAIT_ASSIGNMENT) ||
                 sensor->join.offer_ctr != sec_wire_counter(x->data))) continue;
            if (type == PKT_TYPE_CONFIRM && (!e || mm.admission.pending[0].phase != ADMISSION_EMPTY)) continue;
            if (type == PKT_TYPE_UPLINK && (!e || e->rx.max_ctr < sec_wire_counter(x->data))) continue;
            uint64_t first_rx = sensor->hint_rx, link = e ? e->link_id : 0;
            uint32_t born = mm.admission.pending[0].born_ms, until = mm.admission.join_until_ms, high = e ? e->rx.max_ctr : 0;
            sensor_join_t candidate = sensor->join;
            uint8_t slots = sensor->tx_slots;
            replay_wire(receiver, x->data, x->len);
            if (receiver) sensor->tx_slots = slots;
            assert(sensor->hint_rx == first_rx);
            if (type == PKT_TYPE_JOIN) assert(mm.admission.pending[0].born_ms == born && mm.admission.join_until_ms == until);
            if (type == PKT_TYPE_OFFER) assert(!memcmp(&candidate, &sensor->join, sizeof(candidate)));
            if (type == PKT_TYPE_CONFIRM || type == PKT_TYPE_UPLINK) assert(e->link_id == link && e->rx.max_ctr == high);
            duplicate_types |= 1u << type;
        }
    }
}
static void admission(unsigned scenario)
{
    admission_case = (int)scenario;
    add_node(1, 0, 0, 0);
    unsigned count = scenario >= 6 ? 1 : scenario == 1 ? 4 : 5;
    for (unsigned i = 1; i <= count; i++) add_node(0, 0x10000000u + i, (int32_t)i * 3, 0);
    if (scenario >= 6 && scenario <= 8) nodes[scenario == 7 ? 1 : 0].drop_type =
        scenario == 6 ? PKT_TYPE_JOIN : scenario == 7 ? PKT_TYPE_OFFER : PKT_TYPE_CONFIRM;
    run(scenario == 5 ? 29000000u : 70000000u, admission_hook);
    check_slots(0);
    if (scenario == 11) assert(duplicate_types == 0x3e);
    if (scenario == 5) {
        assert(race_guard && reg_lookup(&mm.reg, nodes[1].sender_id) && !reg_lookup(&mm.reg, nodes[6].sender_id));
    } else {
        for (int i = 1; i < n_nodes; i++) if (!nodes[i].disabled && !(scenario == 4 && i == 6)) {
            assert(nodes[i].s.have_session && nodes[i].s.timing_ready && nodes[i].s.entry >= 0);
            printf("admission %u node%d: registered %.3fs, ready %.3fs\n", scenario, i,
                   first_join[i] / 1e6, first_ready[i] / 1e6);
        }
    }
    if (scenario == 2) {
        assert(nodes[1].s.link_id > old_link);
        unsigned copies = 0;
        for (unsigned i = 0; i < n_recs; i++) if (recs[i].node == nodes[1].sender_id && recs[i].kind == 'C' && recs[i].seq == 100) copies++;
        assert(copies == 1);
    }
    if (scenario == 3) assert(first_join[6] / 1000u >= incumbent_heard + LINK_STALE_MS);
    if (scenario == 4) assert(!first_join[6] && !reg_lookup(&mm.reg, nodes[6].sender_id) && reg_count(&mm.reg) == MAX_NODES);
    /* Each live registered sensor retains a DATA slot throughout admission. */
    for (unsigned a = 0; a < air_n; a++) {
        if (!is_beacon(&air[a]) || air[a].start_us < 19000000u) continue;
        const admission_beacon_t *b = (const admission_beacon_t *)air[a].plain;
        if (!(b->base.flags & BEACON_ADMISSION)) continue;
        for (unsigned k = 0; k < MAX_NODES; k++) {
            if (!b->base.slot[k].node_id || b->base.slot[k].node_id == 0x20000006u) continue;
            if (b->pending & (1u << k)) continue; /* same-ID candidate: reauth assert below */
            unsigned slots = 0;
            for (unsigned j = 0; j < SLOT_COUNT; j++) slots += b->schedule[j] == (SLOT_ROLE_DATA | k);
            assert(slots == 1);
        }
        if (scenario == 2) {
            unsigned slots = 0;
            for (unsigned j = 0; j < SLOT_COUNT; j++) slots += SLOT_ROLE(b->schedule[j]) == SLOT_ROLE_DATA;
            assert(slots == 5);
        }
    }
    printf("PASS admission_%u\n", scenario);
}

/* Reauthentication refreshes link liveness, never cached clock health. */
static uint64_t reauth_old_link;
static uint16_t reauth_old_ack;
static int reauth_health_checked;
static int initial_diag_pending_seen, initial_diag_seen;
static void reauth_health_hook(uint64_t us)
{
    reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
    if (us < 20000000u && e) {
        if (!e->diag_valid) { initial_diag_pending_seen = 1; assert(e->diag.health == 0); }
        else { initial_diag_seen = 1; }
    }
    if (us == 20000000u) {
        assert(e && e->diag.health == HEALTH_EVENT_REQUIRED && e->have_ack);
        reauth_old_link = e->link_id; reauth_old_ack = e->ack_seq;
        nodes[1].disabled = 1;
    }
    if (us == 40000000u) { nodes[1].disabled = 0; nodes[1].xtal = 0; }
    if (us > 40000000u && e && e->link_id != reauth_old_link && !reauth_health_checked) {
        assert(!sync_health(&nodes[1].s.sync, local_tick(&nodes[1], us), nodes[1].xtal));
        assert(e->have_ack && e->ack_seq == reauth_old_ack);
        assert(!e->diag_valid && e->diag.health == 0 && e->diag.sync_age_ms == UINT16_MAX);
        /* The console gets an invalidated D immediately, even without ?STATUS. */
        assert(strstr(last_d, " OK ") && strstr(last_d, " 1 0 0 RC 65535 "));
        assert(strlen(last_d) >= 3 && !strcmp(last_d + strlen(last_d) - 3, " 0\n"));
        reauth_health_checked = 1;
    }
    if (us == 45000000u) { nodes[1].xtal = 1; }
}
static void reauth_health(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    run(65000000u, reauth_health_hook);
    assert(reauth_health_checked && initial_diag_pending_seen && initial_diag_seen);
    reg_entry_t *e = reg_lookup(&mm.reg, nodes[1].sender_id);
    assert(e && e->diag_valid && e->diag.health == HEALTH_EVENT_REQUIRED); /* fresh diagnostics restore health */
    puts("PASS reauth_health");
}

static void join_hint(uint64_t ms)
{
    now_global = ms * 1000u;
    cur = &nodes[0]; cur->now_us = now_global;
    uint8_t wire[WIRE_MAX]; beacon_pl_t b = {0};
    int n = sec_seal(wire, sizeof(wire), PKT_TYPE_BEACON, 0, &b, sizeof(b));
    assert(n > 0);
    replay_wire(1, wire, n);
}
static void join_uptime(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    const uint64_t times[] = {0x7fffffffu, 0x80000000u, UINT32_MAX, UINT64_C(1) << 32};
    for (unsigned i = 0; i < sizeof(times)/sizeof(times[0]); i++) {
        memset(&nodes[1].s.join, 0, sizeof(nodes[1].s.join));
        join_hint(times[i]);
        assert(nodes[1].s.join.phase == SENSOR_JOIN_WAIT_OFFER);
    }
    puts("PASS join_uptime");
}
static void join_backoff(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    uint64_t cancelled_ms = (uint64_t)UINT32_MAX - 1u;
    join_hint(cancelled_ms - JOIN_LIFETIME_MS);
    join_hint(cancelled_ms); /* expire the candidate, start real backoff across wrap */
    assert(nodes[1].s.join.phase == SENSOR_JOIN_IDLE);
    uint32_t wait = nodes[1].s.retry_join_ms - (uint32_t)cancelled_ms;
    assert(wait >= 1000u && wait <= 4000u);
    join_hint(cancelled_ms + wait - 1u);
    assert(nodes[1].s.join.phase == SENSOR_JOIN_IDLE);
    join_hint(cancelled_ms + wait);
    assert(nodes[1].s.join.phase == SENSOR_JOIN_WAIT_OFFER);
    /* A later backoff expires while no beacon asks us to join. Its stale
     * deadline must not become a new wait after another half millis period. */
    cancelled_ms += wait + JOIN_LIFETIME_MS;
    join_hint(cancelled_ms);
    assert(nodes[1].s.join.phase == SENSOR_JOIN_IDLE);
    wait = nodes[1].s.retry_join_ms - (uint32_t)cancelled_ms;
    now_global = (cancelled_ms + wait) * 1000u;
    cur = &nodes[1]; cur->now_us = now_global;
    mac_sensor_step(&cur->s);
    assert(cur->s.join.phase == SENSOR_JOIN_IDLE);
    join_hint(cancelled_ms + wait + (UINT64_C(1) << 31));
    assert(nodes[1].s.join.phase == SENSOR_JOIN_WAIT_OFFER);
    puts("PASS join_backoff");
}

static void join_window_end(void)
{
    add_node(1, 0, 0, 0); add_node(0, 0x123443b0u, 0, 0);
    const uint32_t starts[] = {0, UINT32_MAX - 5000u};
    for (unsigned i = 0; i < 2; i++) {
        master_admission_t a = {0}; registry_t r; reg_init(&r);
        master_admission_session(&a, starts[i]);
        uint32_t arrival = starts[i] + JOIN_WINDOW_MS - 100u;
        join_pl_t j = {.master_boot = nodes[0].boot_id, .ns = {1}};
        uint8_t wire[WIRE_MAX]; uint32_t id;
        cur = &nodes[1];
        int n = sec_seal(wire, sizeof wire, PKT_TYPE_JOIN, cur->sender_id, &j, sizeof j);
        cur = &nodes[0];
        assert(master_admission_receive(&a, &r, wire, n, arrival, &id) == ADMISSION_WAIT);
        admission_beacon_t b = {0}; uint8_t slots[SLOT_COUNT];
        master_admission_schedule(&a, &r, arrival + 100u, &b, slots);
        assert(a.pending[0].phase != ADMISSION_EMPTY);
        uint32_t until = a.join_until_ms;
        assert(master_admission_receive(&a, &r, wire, n, arrival + 1000u, &id) == ADMISSION_IGNORED);
        assert(a.join_until_ms == until && (uint32_t)(until - arrival) >= JOIN_LIFETIME_MS);
        assert(master_admission_offer(&a, 0, 1, &(offer_pl_t){0}));
    }
    puts("PASS join_window_end");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strncmp(argv[1], "admission_", 10)) { admission((unsigned)atoi(argv[1]+10)); }
    else if (!strcmp(argv[1], "reauth_health")) { reauth_health(); }
    else if (!strcmp(argv[1], "join_uptime")) { join_uptime(); }
    else if (!strcmp(argv[1], "join_backoff")) { join_backoff(); }
    else if (!strcmp(argv[1], "steady")) { steady(); }
    else if (!strcmp(argv[1], "radio_delay_3")) { radio_delay(3, 0); }
    else if (!strcmp(argv[1], "radio_delay_5")) { radio_delay(5, 0); }
    else if (!strcmp(argv[1], "radio_asymmetry")) { radio_delay(3, 1); }
    else if (!strcmp(argv[1], "sync_reports_1")) { sync_reports(1, 0); }
    else if (!strcmp(argv[1], "sync_reports_3")) { sync_reports(3, 0); }
    else if (!strcmp(argv[1], "sync_reports_5")) { sync_reports(5, 0); }
    else if (!strcmp(argv[1], "sync_reports_loss")) { sync_reports(1, 1); }
    else if (!strcmp(argv[1], "sync_reports_data")) { sync_reports_data(); }
    else if (!strcmp(argv[1], "sync_reports_retry")) { sync_reports_retry(); }
    else if (!strcmp(argv[1], "roundtrip_binding")) { roundtrip_binding(); }
    else if (!strcmp(argv[1], "status_reports")) { status_reports(); }
    else if (!strcmp(argv[1], "init_recovery")) { init_recovery(); }
    else if (!strcmp(argv[1], "latency")) { latency(1); }
    else if (!strcmp(argv[1], "latency_lbt_off")) { latency(0); }
    else if (!strcmp(argv[1], "lbt_jam")) { lbt_jam(); }
    else if (!strcmp(argv[1], "overflow")) { overflow(); }
    else if (!strcmp(argv[1], "sync_gap")) { sync_gap(); }
    else if (!strcmp(argv[1], "long_gap")) { long_gap(); }
    else if (!strcmp(argv[1], "reboot")) { reboot(1, 2); }
    else if (!strcmp(argv[1], "reboot_new_tag")) { reboot(0, 2); }
    else if (!strcmp(argv[1], "reboot_full")) { reboot(0, 5); }
    else if (!strcmp(argv[1], "ev_wrap")) { ev_wrap(); }
    else if (!strcmp(argv[1], "hfxo_stop")) { hfxo_stop(); }
    else if (!strcmp(argv[1], "xtal_lost")) { xtal_lost(); }
    else if (!strcmp(argv[1], "faults")) { faults(); }
    else if (!strcmp(argv[1], "session_replay")) { session_replay(); }
    else if (!strcmp(argv[1], "late_seal")) { late_seal(); }
    else if (!strcmp(argv[1], "late_slot")) { late_slot(1000u); }
    else if (!strcmp(argv[1], "late_lbt")) { late_slot(6000u); }
    else if (!strcmp(argv[1], "join_window_end")) { join_window_end(); }
    else if (!strcmp(argv[1], "cad_failure")) { cad_failure(); }
    else if (!strcmp(argv[1], "load_2")) { load(2, 3); }
    else if (!strcmp(argv[1], "load_5")) { load(5, 3); }
    else if (!strcmp(argv[1], "load_noise")) { load(5, 20); }
    else { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
    return 0;
}
