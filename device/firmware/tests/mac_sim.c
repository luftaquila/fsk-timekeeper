/* MAC simulator: one master and several sensors on a shared virtual air
 * run the production mac_master.c / mac_sensor.c with the real sync, FIFO,
 * registry, host queue and line formats. Only the hardware boundary is fake:
 * radio (airtime, collisions, loss, half duplex), TIMER1 captures, clocks with
 * ppm error, crypto (a checksum seal) and USB (a host that parses and acks E
 * lines). Each scenario runs in a new process.
 *
 * Invariants checked: sensor transmissions stay inside their slots and never
 * overlap each other or a beacon; every capture reaches the host exactly once
 * and in order, or is covered by a loss range; stamps are within a few ticks of
 * the truth (interpolated ones after a beacon gap too); checkpoints never claim
 * past an undelivered capture.
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

#define SIM_NODES   6
#define AIR_MAX     200000
#define REC_MAX     20000
#define EDGE_MAX    20000
#define STEP_US     200u
/* Worst case: RX, TX and CAD each wait for the TCXO first (RadioLib setTCXO
 * default delay). Standby on the TCXO (radio.cpp) saves some of these waits. */
#define TCXO_US     5000u
#define CAD_US      2560u   /* 4 symbols at SF7/BW250 */
#define TX_SETUP_US 7000u   /* SPI (~1.5 ms at 1 MHz: packet, params) + TCXO + PA ramp */
#define RX_SETUP_US 5500u   /* SPI + TCXO before the receiver listens */
#define HEADER_US   10400u  /* preamble + header before HEADER_VALID */

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

/* ---- nodes and air --------------------------------------------------------- */
typedef struct {
    int src;
    uint64_t start_us, end_us;
    int len;
    uint8_t data[WIRE_MAX];
    uint32_t seen;       /* receivers that evaluated it */
} tx_t;

typedef struct {
    int idx, is_master;
    uint32_t sender_id, boot_id, tx_ctr, rng;
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
    unsigned begin_calls;
    unsigned loss_pct;     /* receive loss, percent */
    int deaf;              /* drops every beacon */
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
static node_t nodes[SIM_NODES];
static int n_nodes;
static node_t *cur;
static uint64_t now_global;
static mq_t mq;
static mac_master_t mm;

static uint64_t local_tick(const node_t *n, uint64_t true_us)
{
    uint64_t t = true_us * 16u;
    int64_t drift = (int64_t)t * n->ppm / 1000000;
    return n->tick_base + t + (uint64_t)drift;
}

static uint32_t airtime_us(int len)
{
    unsigned sym = 8u + (unsigned)((8 * len + 16 + 27) / 28) * 5u;
    return sym * 512u + 6272u; /* + (8 + 4.25) preamble symbols */
}

static int is_beacon(const tx_t *x) { return SEC_VT_TYPE(x->data[0]) == PKT_TYPE_BEACON; }

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
        if (x->src == r->idx || (x->seen >> r->idx) & 1u || x->end_us > r->now_us) { continue; }
        x->seen |= 1u << r->idx;
        if (!r->rx_on || r->rx_since_us > x->start_us || collided(i)) { continue; }
        if (mix(i * 131u + (unsigned)r->idx) % 100u < r->loss_pct) { continue; }
        if (r->deaf && is_beacon(x)) { continue; }
        memcpy(r->pkt, x->data, (size_t)x->len);
        r->pkt_len = x->len;
        r->have_pkt = 1;
        r->dio1 = 1;
        r->dio1_tick = local_tick(r, x->end_us);
    }
}

static const tx_t *on_air(const node_t *r)
{
    unsigned from = air_n > 64u ? air_n - 64u : 0u;
    for (unsigned i = from; i < air_n; i++) {
        const tx_t *x = &air[i];
        if (x->src != r->idx && x->start_us <= r->now_us && x->end_us > r->now_us) { return x; }
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
    cur->noresp = 0;
    cur->rx_on = 0;
    cur->have_pkt = 0;
    return 0;
}

int radio_start_rx(void)
{
    deliver(cur);
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

int radio_transmit(const uint8_t *data, int len)
{
    deliver(cur);
    cur->rx_on = 0;
    cur->have_pkt = 0;
    assert(air_n < AIR_MAX);
    tx_t *x = &air[air_n++];
    x->src = cur->idx;
    x->start_us = cur->now_us + TX_SETUP_US;
    x->end_us = cur->now_us + airtime_us(len);
    x->len = len;
    x->seen = 0;
    memcpy(x->data, data, (size_t)len);
    cur->now_us = x->end_us;
    cur->dio1 = 1;
    cur->dio1_tick = local_tick(cur, cur->now_us);
    return 0;
}

int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr)
{
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
    if (max_ms == 0 || x->end_us > cur->now_us + (uint64_t)max_ms * 1000u) {
        if (max_ms) { cur->now_us += (uint64_t)max_ms * 1000u; }
        return RADIO_RX_BUSY;
    }
    cur->now_us = x->end_us;
    deliver(cur);
    return cur->have_pkt ? RADIO_RX_PACKET : RADIO_RX_IDLE;
}

int radio_cad(void)
{
    deliver(cur);
    uint64_t t0 = cur->now_us + TCXO_US; /* listens only after the TCXO start */
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

int radio_needs_reset(void) { return cur->noresp >= (int)RADIO_NORESP_RESET; }

/* A checksum stands in for the AEAD: version, length and integrity checks only. */
int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id, const void *payload, int payload_len)
{
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (out_cap < wire) { return -1; }
    uint32_t ctr = ++cur->tx_ctr;
    out[0] = SEC_VT(PROTO_VER, type);
    memcpy(out + 1, &cur->boot_id, 4);
    out[5] = (uint8_t)ctr; out[6] = (uint8_t)(ctr >> 8); out[7] = (uint8_t)(ctr >> 16);
    if (has_node) { memcpy(out + 8, &node_id, 4); }
    memcpy(out + hdr, payload, (size_t)payload_len);
    uint32_t c = crc32_ieee(out, (uint32_t)(hdr + payload_len));
    for (int i = 0; i < SEC_MAC_LEN; i++) { out[hdr + payload_len + i] = (uint8_t)(c >> (8 * (i % 4))); }
    return wire;
}

int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta, void *out_payload, int payload_len)
{
    if (in_len < 1) { return -1; }
    if (SEC_VT_VER(in[0]) != PROTO_VER) { return -4; }
    uint8_t type = SEC_VT_TYPE(in[0]);
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    if (in_len < hdr + payload_len + SEC_MAC_LEN) { return -1; }
    uint32_t c = crc32_ieee(in, (uint32_t)(hdr + payload_len));
    for (int i = 0; i < SEC_MAC_LEN; i++) { if (in[hdr + payload_len + i] != (uint8_t)(c >> (8 * (i % 4)))) { return -2; } }
    meta->type = type;
    memcpy(&meta->boot_id, in + 1, 4);
    meta->ctr = (uint32_t)in[5] | ((uint32_t)in[6] << 8) | ((uint32_t)in[7] << 16);
    meta->node_id = NODE_MASTER;
    if (has_node) { memcpy(&meta->node_id, in + 8, 4); }
    memcpy(out_payload, in + hdr, (size_t)payload_len);
    return 0;
}

int sec_authentic_any_version(const uint8_t *in, int in_len) { (void)in; (void)in_len; return 0; }

int sec_replay(sec_replay_t *st, uint32_t boot_id, uint32_t ctr)
{
    if (!st->have || boot_id != st->boot_id) { st->have = 1; st->boot_id = boot_id; st->max_ctr = ctr; return 1; }
    if (ctr > st->max_ctr) { st->max_ctr = ctr; return 1; }
    return 0;
}

uint32_t sec_boot_id(void) { return cur->boot_id; }
int sec_provisioned(void) { return 1; }
uint32_t sec_random(void) { cur->rng = mix(cur->rng + 0x9E3779B9u); return cur->rng; }

/* ---- host: parses E lines, checks their crc, acks ------------------------ */
typedef struct {
    uint32_t hseq, node, sboot, mboot;
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

int usb_write(const char *s)
{
    if (s[0] == 'D') { d_lines++; }
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
    uint32_t mboot = (uint32_t)strtoul(tok[6], NULL, 10);
    if (host_acks) { (void)mq_ack(&mq, hseq, mboot, crc); }
    if (hseq <= last_hseq) { return; } /* a re-send */
    assert(hseq == last_hseq + 1u);    /* nothing skipped */
    last_hseq = hseq;
    rec_t r = { .hseq = hseq, .node = !strcmp(tok[2], "0") ? 0u : (uint32_t)strtoul(tok[2], NULL, 16),
                .kind = tok[3][0], .flags = (unsigned)strtoul(tok[5], NULL, 10), .mboot = mboot,
                .sboot = (uint32_t)strtoul(tok[7], NULL, 10), .seq = (uint32_t)strtoul(tok[11], NULL, 10),
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
        assert(r.kind == 'K' && nt == 14);
        r.end_seq = r.seq;
        r.tick = r.end_tick = strtoull(tok[12], NULL, 10);
        recs[n_recs++] = r;
    }
}

/* ---- simulation --------------------------------------------------------- */
typedef struct { int node; uint64_t us; int injected; uint32_t seq; } edge_t;
static edge_t edges[EDGE_MAX];
static unsigned n_edges;

static void add_node(int is_master, uint32_t id, int32_t ppm, unsigned loss_pct)
{
    node_t *n = &nodes[n_nodes];
    memset(n, 0, sizeof(*n));
    n->idx = n_nodes++;
    n->is_master = is_master;
    n->sender_id = id;
    n->boot_id = mix(id * 7919u + 17u);
    n->rng = mix(id);
    n->ppm = ppm;
    n->tick_base = (uint64_t)mix(id) * 1000u;
    n->xtal = 1;
    n->loss_pct = loss_pct;
    n->now_us = (uint64_t)n->idx * 137u; /* sensors do not start in lockstep */
    cur = n;
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
    unsigned next_edge = 0;
    qsort(edges, n_edges, sizeof(edges[0]), cmp_edge);
    for (; now_global < end_us; now_global += STEP_US) {
        if (hook) { hook(now_global); }
        while (next_edge < n_edges && edges[next_edge].us <= now_global) {
            edge_t *e = &edges[next_edge++];
            isr_edge(&nodes[e->node], e->us);
            e->injected = 1;
            e->seq = nodes[e->node].cseq;
        }
        for (int i = 0; i < n_nodes; i++) {
            node_t *n = &nodes[i];
            if (n->now_us > now_global) { continue; }
            n->now_us = now_global;
            cur = n;
            if (n->is_master) {
                (void)mac_master_step(&mm);
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
    int have_beacon = 0;
    for (unsigned i = 0; i < air_n; i++) {
        const tx_t *x = &air[i];
        if (is_beacon(x)) {
            last_beacon_end = x->end_us;
            have_beacon = 1;
            continue;
        }
        if (x->start_us < from_us || !have_beacon) { continue; }
        /* inside one slot of the beacon it follows */
        uint64_t off = x->start_us - last_beacon_end;
        assert(off >= SLOT_OFFSET_MS * 1000u);
        uint64_t k = (off - SLOT_OFFSET_MS * 1000u) / (SLOT_LEN_MS * 1000u);
        assert(k < MAX_NODES);
        uint64_t slot_start = last_beacon_end + (SLOT_OFFSET_MS + k * SLOT_LEN_MS) * 1000u;
        assert(x->start_us - slot_start <= (SLOT_LATE_MS + 1u) * 1000u + TX_SETUP_US);
        assert(x->end_us <= slot_start + SLOT_LEN_MS * 1000u);
        for (unsigned j = 0; j < air_n; j++) {
            if (j == i) { continue; }
            assert(!(air[j].start_us < x->end_us && air[j].end_us > x->start_us)); /* no overlap at all */
        }
    }
}

/* Every injected capture after `from_us` of `node`: exactly once, in order, as a
 * capture within tol ticks of the truth or inside a loss range. */
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

/* Stamps: the skew estimate has 1 ppm resolution and is applied up to
 * SYNC_TTL_MS past its anchor (<= 112 ticks), interpolation is exact to a tick. */
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

/* Sensor 1 reboots. With the same low byte (the beacon's boot tag) it takes its
 * slot back at once and only the ACK check protects it; with a new tag it
 * contends again, in the one slot under its short id when none is free. */
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
    unsigned interp, lost;
    for (int i = 2; i <= sensors; i++) { check_records(i, 12000000u, TOL_TICKS, &interp, &lost); }
    /* after the reboot every capture of the new boot arrives */
    uint32_t new_boot = nodes[1].boot_id;
    unsigned got = 0, want = 0;
    for (unsigned i = 0; i < n_edges; i++) { if (edges[i].node == 1 && edges[i].us >= 52000000u) { want++; } }
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].node == nodes[1].sender_id && recs[j].sboot == new_boot && recs[j].kind == 'C') { got++; }
        if (recs[j].node == nodes[1].sender_id && recs[j].sboot == new_boot) { assert(recs[j].kind != 'L'); }
    }
    assert(got == want && want > 0);
    printf("PASS reboot (%s tag, %d sensors)\n", same_tag ? "same" : "new", sensors);
}

/* Two sensors whose ids share the low 16 bits: the master registers one and
 * refuses the other, which must not jam the registered one's slot. */
static void id_collision(void)
{
    add_node(1, 0, 0, 0);
    add_node(0, 0x0D2243B0u, 13, 0);
    add_node(0, 0x7A1C43B0u, -9, 0);
    add_node(0, 0x11110003u, 4, 0);
    for (int s = 1; s <= 3; s++) { random_edges(s, 12000000u, 90000000u, 2500, (uint32_t)s * 31u); }
    run(100000000u, NULL);
    check_slots(15000000u);
    unsigned got[4] = { 0 };
    for (unsigned j = 0; j < n_recs; j++) {
        for (int s = 1; s <= 3; s++) { if (recs[j].node == nodes[s].sender_id) { got[s]++; } }
    }
    assert((got[1] == 0) != (got[2] == 0) && got[3] > 0);
    assert(el_count(EL_ID_COLLISION) > 0);
    unsigned interp, lost;
    check_records(got[1] ? 1 : 2, 12000000u, TOL_TICKS, &interp, &lost);
    check_records(3, 12000000u, TOL_TICKS, &interp, &lost);
    assert(lost == 0);
    printf("PASS id_collision\n");
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

/* A masked HFXO stop drops the sync: captures until fresh anchors and skew are
 * of unknown time, later ones are stamped again. */
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

static void faults_hook(uint64_t us)
{
    if (us == 30000000u) { nodes[2].noresp = (int)RADIO_NORESP_RESET; }
    if (us == 45000000u) {
        /* master timebase session change (clock fault recovery path) */
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
    edge_at(1, 60000000u);
    edge_at(2, 61000000u);
    run(80000000u, faults_hook);
    assert(nodes[2].begin_calls == 1); /* three SPI no-responses reset the radio */
    int saw_end = 0;
    unsigned after = 0;
    for (unsigned j = 0; j < n_recs; j++) {
        if (recs[j].node == 0) { saw_end = 1; assert(recs[j].kind == 'L'); }
        if (recs[j].kind == 'C') { assert(recs[j].mboot == nodes[0].boot_id); after++; }
    }
    assert(saw_end && after == 2);
    printf("PASS faults\n");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "steady")) { steady(); }
    else if (!strcmp(argv[1], "overflow")) { overflow(); }
    else if (!strcmp(argv[1], "sync_gap")) { sync_gap(); }
    else if (!strcmp(argv[1], "long_gap")) { long_gap(); }
    else if (!strcmp(argv[1], "reboot")) { reboot(1, 2); }
    else if (!strcmp(argv[1], "reboot_new_tag")) { reboot(0, 2); }
    else if (!strcmp(argv[1], "reboot_full")) { reboot(0, 5); }
    else if (!strcmp(argv[1], "id_collision")) { id_collision(); }
    else if (!strcmp(argv[1], "ev_wrap")) { ev_wrap(); }
    else if (!strcmp(argv[1], "hfxo_stop")) { hfxo_stop(); }
    else if (!strcmp(argv[1], "faults")) { faults(); }
    else { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
    return 0;
}
