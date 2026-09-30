/* Sensor FIFO, host queue, registry and the USB line formats, with the USB and
 * clock boundary faked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/crc32.h"
#include "../src/errlog.h"
#include "../src/master_queue.h"
#include "../src/proto_usb.h"
#include "../src/registry.h"
#include "../src/sensor_queue.h"
#include "../src/sync.h"

/* ---- fakes -------------------------------------------------------------- */
static uint32_t now_ms;
static int usb_ok = 1;
static char usb_out[64][PU_LINE_MAX + 8];
static unsigned usb_n;

uint32_t board_millis(void) { return now_ms; }
int usb_write(const char *s)
{
    if (!usb_ok) { return 0; }
    assert(usb_n < 64);
    strcpy(usb_out[usb_n++], s);
    return 1;
}

static void usb_reset(void) { usb_n = 0; }

/* ---- sensor FIFO ---------------------------------------------------------- */
static void fifo_bundles(void)
{
    static sensor_queue_t q;
    sq_init(&q);
    for (uint32_t s = 1; s <= 7; s++) { sq_push_edge(&q, s, 1000u * s, HEALTH_EVENT_REQUIRED, 100); }
    uplink_pl_t *p = sq_packet(&q, 42);
    assert(p && p->kind == UL_KIND_EDGES && p->u.edges.count == 5);
    assert(p->ev_seq == 1 && p->capture_seq == 1 && p->tick == 1000 && p->master_boot_id == 42);
    for (unsigned i = 0; i < 4; i++) { assert(p->u.edges.dt[i] == 1000); }
    /* the in-flight packet is re-sent unchanged until its ev_seq is acked */
    assert(sq_packet(&q, 42) == p && p->ev_seq == 1);
    sq_ack(&q, 0);
    assert(q.inflight && q.count == 7);
    sq_ack(&q, 1);
    assert(!q.inflight && q.count == 2);
    p = sq_packet(&q, 42);
    assert(p->ev_seq == 2 && p->u.edges.count == 2 && p->capture_seq == 6);
    sq_ack(&q, 2);
    assert(sq_idle(&q) && sq_packet(&q, 42) == NULL);

    /* bundles stop at a flag change, a seq gap, a loss and a held capture */
    sq_push_edge(&q, 10, 100, HEALTH_EVENT_REQUIRED, 0);
    sq_push_edge(&q, 11, 200, HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED, 0);
    sq_push_edge(&q, 13, 300, HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED, 0);
    sq_push_loss(&q, 14, 15, 400, 500, HEALTH_EVENT_REQUIRED);
    sq_push_held(&q, 16, 77);
    p = sq_packet(&q, 1); assert(p->u.edges.count == 1 && p->capture_seq == 10); sq_ack(&q, p->ev_seq);
    p = sq_packet(&q, 1); assert(p->u.edges.count == 1 && p->capture_seq == 11); sq_ack(&q, p->ev_seq);
    p = sq_packet(&q, 1); assert(p->u.edges.count == 1 && p->capture_seq == 13); sq_ack(&q, p->ev_seq);
    p = sq_packet(&q, 1);
    assert(p->kind == UL_KIND_LOSS && p->capture_seq == 14 && p->u.loss.end_seq == 15 && p->tick == 400 && p->u.loss.end_tick == 500);
    sq_ack(&q, p->ev_seq);
    assert(sq_packet(&q, 1) == NULL); /* held head blocks until stamped */
    assert(sq_checkpoint(&q, 1, 16, 999, HEALTH_EVENT_REQUIRED, 0) == NULL); /* not idle */
    printf("PASS fifo_bundles\n");
}

static void fifo_overflow_keeps_order(void)
{
    static sensor_queue_t q;
    sq_init(&q);
    uint32_t seq = 1;
    for (; seq <= SENSOR_FIFO_LEN + 10u; seq++) { sq_push_edge(&q, seq, 16000u * seq, HEALTH_EVENT_REQUIRED, 0); }
    assert(q.count == SENSOR_FIFO_LEN && q.acc_pending);
    assert(q.acc.kind == SQ_LOSS && q.acc.seq == SENSOR_FIFO_LEN + 1u && q.acc.end_seq == SENSOR_FIFO_LEN + 10u);
    assert(q.acc.tick == 16000u * (SENSOR_FIFO_LEN + 1u) && !(q.acc.flags & EVENT_TIME_UNKNOWN));
    assert(el_count(EL_FIFO_FULL) == 1);
    /* a held capture joining the range makes its time unknown */
    sq_push_held(&q, seq++, 5);
    assert(q.acc.flags == EVENT_TIME_UNKNOWN && q.acc.end_seq == SENSOR_FIFO_LEN + 11u);
    /* drain everything: every seq appears exactly once, in order */
    uint32_t expect = 1;
    for (int guard = 0; guard < 1000 && !sq_idle(&q); guard++) {
        if (guard == 3) { sq_push_edge(&q, seq, 16000u * seq, HEALTH_EVENT_REQUIRED, 0); seq++; }
        uplink_pl_t *p = sq_packet(&q, 7);
        assert(p);
        assert(p->capture_seq == expect);
        if (p->kind == UL_KIND_EDGES) { expect += p->u.edges.count; }
        else { assert(p->kind == UL_KIND_LOSS); expect = p->u.loss.end_seq + 1u; }
        sq_ack(&q, p->ev_seq);
    }
    assert(sq_idle(&q) && expect == seq);
    printf("PASS fifo_overflow_keeps_order\n");
}

static void fifo_range_never_shrinks(void)
{
    static sensor_queue_t q;
    sq_init(&q);
    uint32_t seq = 1;
    for (; seq <= SENSOR_FIFO_LEN; seq++) { sq_push_edge(&q, seq, 16000u * seq, HEALTH_EVENT_REQUIRED, 0); }
    sq_push_loss(&q, seq, seq + 3u, 1u, 2u, HEALTH_EVENT_REQUIRED);
    /* a record behind the range's end must not cut the range back */
    sq_push_edge(&q, seq + 2u, 3u, HEALTH_EVENT_REQUIRED, 0);
    assert(q.acc_pending && q.acc.seq == seq && q.acc.end_seq == seq + 3u && q.acc.end_tick == 2u);
    uint32_t expect = 1;
    for (int guard = 0; guard < 1000 && !sq_idle(&q); guard++) {
        uplink_pl_t *p = sq_packet(&q, 7);
        assert(p && p->capture_seq == expect);
        expect = p->kind == UL_KIND_EDGES ? expect + p->u.edges.count : p->u.loss.end_seq + 1u;
        sq_ack(&q, p->ev_seq);
    }
    assert(expect == seq + 4u);
    printf("PASS fifo_range_never_shrinks\n");
}

static void fifo_held_and_clear(void)
{
    static sensor_queue_t q;
    sq_init(&q);
    sync_t s;
    memset(&s, 0, sizeof(s));
    s.cur.have = 1;
    s.cur.local = 1000000;
    s.cur.off = 5000;
    s.skew_valid = 1;
    sq_push_held(&q, 1, 1000000u + 20u * TICKS_PER_MS * 1000u); /* 20 s after the anchor */
    sq_resolve(&q, &s, 1000000u + 21u * TICKS_PER_MS * 1000u);
    assert(q.item[q.head].kind == SQ_HELD); /* still waiting for the next anchor */
    s.before = s.cur;
    s.cur.local = 1000000u + 30u * TICKS_PER_MS * 1000u;
    s.cur.off = 5000u + 10u;
    sq_resolve(&q, &s, s.cur.local);
    assert(q.item[q.head].kind == SQ_EDGE && q.item[q.head].flags == (HEALTH_EVENT_REQUIRED | EVENT_INTERPOLATED));
    assert(q.item[q.head].sync_age_ms == 20000u);
    /* interpolated between off 5000 (t=0) and 5010 (t=30 s): +20/30 of 10 */
    assert(q.item[q.head].tick == 1000000u + 20u * TICKS_PER_MS * 1000u + 5000u + 7u);

    /* a held capture whose anchor aged past the limit becomes a loss */
    sq_push_held(&q, 2, s.cur.local + 8u * 1000u * TICKS_PER_MS);
    sq_resolve(&q, &s, s.cur.local + (SYNC_HOLD_MAX_MS + 1u) * TICKS_PER_MS);
    const sq_item_t *it = &q.item[(q.head + 1u) % SENSOR_FIFO_LEN];
    assert(it->kind == SQ_LOSS && it->flags == EVENT_TIME_UNKNOWN && it->seq == 2 && it->end_seq == 2);

    sq_clear(&q);
    assert(sq_idle(&q) && q.fifo_drop == 2);
    uplink_pl_t *p = sq_checkpoint(&q, 3, 9, 12345, HEALTH_EVENT_REQUIRED, 12);
    assert(p && p->kind == UL_KIND_CHECKPOINT && p->capture_seq == 9 && p->tick == 12345 && p->ev_seq == 1);
    q.next_ev_seq = 0xFFFFu;
    sq_ack(&q, 1);
    p = sq_checkpoint(&q, 3, 9, 1, 0, 0);
    assert(p->ev_seq == 0xFFFFu && q.next_ev_seq == 1); /* 0 is never used */
    printf("PASS fifo_held_and_clear\n");
}

/* ---- host queue + E lines -------------------------------------------------- */
static void line_crc_ok(const char *line)
{
    const char *sp = strrchr(line, ' ');
    assert(sp && strlen(sp + 1) == 9 && sp[9] == '\n');
    char hex[9];
    memcpy(hex, sp + 1, 8);
    hex[8] = '\0';
    uint32_t crc = (uint32_t)strtoul(hex, NULL, 16);
    assert(crc == crc32_ieee(line, (uint32_t)(sp - line)));
}

static void host_queue(void)
{
    static mq_t q;
    mq_init(&q);
    uplink_pl_t u;
    memset(&u, 0, sizeof(u));
    u.kind = UL_KIND_EDGES;
    u.flags = HEALTH_EVENT_REQUIRED;
    u.ev_seq = 1234;
    u.master_boot_id = 3735928559u;
    u.capture_seq = 42;
    u.tick = 123456789012345678ull;
    u.sync_age_ms = 120;
    u.u.edges.count = 2;
    u.u.edges.dt[0] = 16000000u;
    assert(mq_push_uplink(&q, 0x0D2243B0u, 305419896u, &u, -91.5f, 9.25f));
    const char *want = "E 1 0D2243B0 C 1234 7 3735928559 305419896 120 -91.50 9.25 42 2 123456789012345678 123456789028345678";
    assert(!strncmp(q.item[0].line, want, strlen(want)));
    line_crc_ok(q.item[0].line);

    u.kind = UL_KIND_LOSS;
    u.flags = EVENT_TIME_UNKNOWN;
    u.u.loss.end_seq = 45;
    u.u.loss.end_tick = 0;
    u.tick = 0;
    assert(mq_push_uplink(&q, 0x7A1C9F02u, 7u, &u, 0.0f, 0.0f));
    assert(!strncmp(q.item[1].line, "E 2 7A1C9F02 L 1234 64 3735928559 7 120 0.00 0.00 42 45 0 0 ", 60));
    line_crc_ok(q.item[1].line);

    u.kind = UL_KIND_CHECKPOINT;
    u.flags = HEALTH_EVENT_REQUIRED;
    u.tick = 99;
    assert(mq_push_uplink(&q, 0x7A1C9F02u, 7u, &u, -60.0f, 7.0f));
    assert(!strncmp(q.item[2].line, "E 3 7A1C9F02 K 1234 7 3735928559 7 120 -60.00 7.00 42 99 ", 57));
    assert(mq_push_timebase_end(&q, 11u, 5000u));
    assert(!strncmp(q.item[3].line, "E 4 0 L 0 0 11 11 0 0.00 0.00 0 0 5000 5000 ", 44));
    line_crc_ok(q.item[3].line);

    /* the head is re-sent unchanged every MASTER_USB_RETRY_MS until acked */
    usb_reset();
    now_ms = 1000;
    mq_pump(&q, now_ms);
    mq_pump(&q, now_ms + 50u);
    mq_pump(&q, now_ms + MASTER_USB_RETRY_MS);
    assert(usb_n == 2 && !strcmp(usb_out[0], usb_out[1]) && !strcmp(usb_out[0], q.item[0].line));
    uint32_t crc = q.item[0].crc;
    assert(!mq_ack(&q, 1, 3735928559u, crc ^ 1u) && q.ack_mismatch == 1 && mq_depth(&q) == 4);
    assert(!mq_ack(&q, 1, 1u, crc) && q.ack_mismatch == 2);
    assert(mq_ack(&q, 1, 3735928559u, crc) && mq_depth(&q) == 3);
    assert(!mq_ack(&q, 1, 3735928559u, crc) && q.ack_mismatch == 2); /* repeat of a popped line */
    /* full: refused and counted, nothing overwritten */
    while (!mq_full(&q)) { assert(mq_push_timebase_end(&q, 1u, 1u)); }
    assert(!mq_push_timebase_end(&q, 1u, 1u) && q.overflow == 1);
    printf("PASS host_queue\n");
}

static void ack_parser(void)
{
    const char *good = "C 17 3735928559 1a2B3c4D\n";
    pu_cmd_t cmd = PU_CMD_NONE;
    for (const char *p = good; *p; p++) { cmd = pu_feed(*p); }
    assert(cmd == PU_CMD_EVENT_ACK && pu_ack_hseq() == 17 && pu_ack_boot() == 3735928559u && pu_ack_crc() == 0x1A2B3C4Du);
    const char *bad[] = { "C 17 3735928559 1A2B3C4\n", "C 17 4294967296 1A2B3C4D\n", "C x 1 1A2B3C4D\n", "C 1 2 1A2B3C4DZ\n" };
    for (unsigned i = 0; i < 4; i++) {
        cmd = PU_CMD_NONE;
        for (const char *p = bad[i]; *p; p++) { cmd = pu_feed(*p); }
        assert(cmd == PU_CMD_BAD);
    }
    printf("PASS ack_parser\n");
}

static void control_retry(void)
{
    usb_reset();
    usb_ok = 0;
    now_ms = 5000;
    pu_emit_ack("PING");       /* control: queued */
    pu_emit_heartbeat(1, 2, 3, 4); /* periodic: dropped */
    assert(usb_n == 0 && pu_tx_drop() == 1);
    uint32_t drops = el_count(EL_USB_DROP);
    usb_ok = 1;
    now_ms += 10;
    pu_service(now_ms);
    assert(usb_n == 1 && !strcmp(usb_out[0], "A PING OK\n"));
    usb_ok = 0;
    pu_emit_ack("ID");
    now_ms += 1001;
    usb_ok = 1;
    pu_service(now_ms);
    assert(usb_n == 1 && el_count(EL_USB_DROP) == drops + 1);
    printf("PASS control_retry\n");
}

/* ---- registry ----------------------------------------------------------- */
static void registry(void)
{
    registry_t r;
    reg_init(&r);
    int why = 0;
    reg_entry_t *a = reg_claim(&r, 0x00010001u, 0, &why);
    assert(a && a == &r.e[0] && a->short_id == 1);
    a->have_boot = 1; a->sensor_boot = 0x1234ABu; a->ack_seq = 9;
    assert(reg_lookup(&r, 0x00010001u) == a);
    /* same short id as a live sensor: refused */
    assert(!reg_claim(&r, 0x00020001u, 0, &why) && why == REG_COLLISION);
    for (uint32_t i = 2; i <= MAX_NODES; i++) { assert(reg_claim(&r, i, 0, &why)); }
    assert(!reg_claim(&r, 0x77u, 0, &why) && why == REG_FULL);
    beacon_slot_t t[MAX_NODES];
    reg_slot_table(&r, 1000, t);
    assert(t[0].short_id == 1 && t[0].ack_seq == 9 && t[0].boot_tag == 0xAB);
    /* a LOST sensor frees its slot in the table, and a newcomer may take it */
    for (unsigned i = 1; i < MAX_NODES; i++) { r.e[i].heard_ms = LINK_STALE_MS + 1u; }
    reg_slot_table(&r, LINK_STALE_MS + 2u, t);
    assert(t[0].short_id == 0 && t[1].short_id == 2);
    assert(reg_state(&r.e[1], LINK_STALE_MS + 2u) == PU_STATE_OK);
    assert(reg_state(&r.e[1], LINK_STALE_MS + 1u + LINK_OK_MS + 1u) == PU_STATE_STALE);
    reg_entry_t *n = reg_claim(&r, 0x00020001u, LINK_STALE_MS + 2u, &why);
    assert(n == &r.e[0] && n->short_id == 1 && !n->have_boot);
    assert(node_short_id(0x00050000u) == 5 && node_short_id(0x12340000u) == 0x1234 && node_short_id(0x10000u) == 1);
    printf("PASS registry\n");
}

int main(void)
{
    fifo_bundles();
    fifo_overflow_keeps_order();
    fifo_range_never_shrinks();
    fifo_held_and_clear();
    host_queue();
    ack_parser();
    control_retry();
    registry();
    return 0;
}
