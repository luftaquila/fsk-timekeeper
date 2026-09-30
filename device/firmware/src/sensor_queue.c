#include "sensor_queue.h"

#include <string.h>

#include "errlog.h"

void sq_init(sensor_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->next_ev_seq = 1;
}

static void count_drop(sensor_queue_t *q, unsigned n)
{
    unsigned v = q->fifo_drop + n;
    q->fifo_drop = (uint16_t)(v > UINT16_MAX ? UINT16_MAX : v);
}

void sq_clear(sensor_queue_t *q)
{
    count_drop(q, q->count + (q->acc_pending ? 1u : 0u));
    q->head = q->count = 0;
    q->acc_pending = 0;
    q->inflight = 0;
    q->pkt_items = 0;
}

static sq_item_t *at(sensor_queue_t *q, unsigned i)
{
    return &q->item[(q->head + i) % SENSOR_FIFO_LEN];
}

static void as_loss(sq_item_t *it)
{
    if (it->kind == SQ_LOSS) { return; }
    if (it->kind == SQ_HELD) { it->flags = EVENT_TIME_UNKNOWN; }
    it->kind = SQ_LOSS;
    it->end_seq = it->seq;
    it->end_tick = it->tick;
}

/* Extend the waiting loss range with a later record. */
static void acc_merge(sensor_queue_t *q, sq_item_t it)
{
    as_loss(&it);
    if (!q->acc_pending) {
        q->acc = it;
        q->acc_pending = 1;
        el_note(EL_FIFO_FULL);
        return;
    }
    /* Records arrive in seq order; should one not, the range still must not
     * shrink below a seq it already covers, or that seq would never be sent. */
    if ((int32_t)(it.end_seq - q->acc.end_seq) > 0) {
        q->acc.end_seq = it.end_seq;
        q->acc.end_tick = it.end_tick;
    }
    if ((q->acc.flags | it.flags) & EVENT_TIME_UNKNOWN) { q->acc.flags = EVENT_TIME_UNKNOWN; }
    else { q->acc.flags &= it.flags; }
}

static void flush_acc(sensor_queue_t *q)
{
    if (q->acc_pending && q->count < SENSOR_FIFO_LEN) {
        *at(q, q->count) = q->acc;
        q->count++;
        q->acc_pending = 0;
    }
}

static void push(sensor_queue_t *q, const sq_item_t *it)
{
    flush_acc(q);
    if (q->acc_pending || q->count >= SENSOR_FIFO_LEN) {
        acc_merge(q, *it);
        return;
    }
    *at(q, q->count) = *it;
    q->count++;
}

void sq_push_edge(sensor_queue_t *q, uint32_t seq, uint64_t master, uint8_t flags, uint16_t sync_age_ms)
{
    sq_item_t it = { .kind = SQ_EDGE, .flags = flags, .sync_age_ms = sync_age_ms,
                     .seq = seq, .end_seq = seq, .tick = master, .end_tick = master };
    push(q, &it);
}

void sq_push_held(sensor_queue_t *q, uint32_t seq, uint64_t local)
{
    sq_item_t it = { .kind = SQ_HELD, .flags = 0, .sync_age_ms = UINT16_MAX,
                     .seq = seq, .end_seq = seq, .tick = local, .end_tick = local };
    push(q, &it);
}

void sq_push_loss(sensor_queue_t *q, uint32_t first_seq, uint32_t last_seq,
                  uint64_t first_tick, uint64_t last_tick, uint8_t flags)
{
    sq_item_t it = { .kind = SQ_LOSS, .flags = flags, .sync_age_ms = 0,
                     .seq = first_seq, .end_seq = last_seq, .tick = first_tick, .end_tick = last_tick };
    push(q, &it);
}

void sq_resolve(sensor_queue_t *q, const sync_t *s, uint64_t now_local)
{
    int expired = !s->cur.have || now_local < s->cur.local ||
                  (now_local - s->cur.local) > (uint64_t)SYNC_HOLD_MAX_MS * TICKS_PER_MS;
    for (unsigned i = 0; i < q->count; i++) {
        sq_item_t *it = at(q, i);
        if (it->kind != SQ_HELD) { continue; }
        uint64_t master;
        uint8_t flags;
        int r = sync_stamp(s, it->tick, 1, &master, &flags);
        if (r == SYNC_STAMPED && (flags & HEALTH_EVENT_REQUIRED) == HEALTH_EVENT_REQUIRED) {
            uint64_t ref = (flags & EVENT_INTERPOLATED) ? s->before.local : s->cur.local;
            uint64_t age = (it->tick - ref) / TICKS_PER_MS;
            it->kind = SQ_EDGE;
            it->flags = flags;
            it->sync_age_ms = (uint16_t)(age > UINT16_MAX ? UINT16_MAX : age);
            it->tick = it->end_tick = master;
        } else if (r != SYNC_HOLD || expired) {
            as_loss(it);
            it->flags = EVENT_TIME_UNKNOWN;
        }
    }
}

int sq_idle(const sensor_queue_t *q)
{
    return q->count == 0 && !q->acc_pending && !q->inflight;
}

static void take_ev_seq(sensor_queue_t *q)
{
    q->pkt.ev_seq = q->next_ev_seq;
    if (++q->next_ev_seq == 0) { q->next_ev_seq = 1; }
    q->inflight = 1;
}

uplink_pl_t *sq_packet(sensor_queue_t *q, uint32_t master_boot_id)
{
    if (q->inflight) { return &q->pkt; }
    flush_acc(q);
    if (q->count == 0) { return NULL; }
    const sq_item_t *first = at(q, 0);
    if (first->kind == SQ_HELD) { return NULL; }

    memset(&q->pkt, 0, sizeof(q->pkt));
    q->pkt.master_boot_id = master_boot_id;
    q->pkt.flags = first->flags;
    q->pkt.capture_seq = first->seq;
    q->pkt.tick = first->tick;
    q->pkt.sync_age_ms = first->sync_age_ms;
    if (first->kind == SQ_LOSS) {
        /* Consecutive losses of unknown time go as one range, not one per cycle. */
        unsigned n = 1;
        const sq_item_t *last = first;
        while (first->flags == EVENT_TIME_UNKNOWN && n < q->count) {
            const sq_item_t *it = at(q, n);
            if (it->kind != SQ_LOSS || it->flags != EVENT_TIME_UNKNOWN || it->seq != last->end_seq + 1u) { break; }
            last = it;
            n++;
        }
        q->pkt.kind = UL_KIND_LOSS;
        q->pkt.u.loss.end_seq = last->end_seq;
        q->pkt.u.loss.end_tick = last->end_tick;
        q->pkt_items = n;
    } else {
        /* Bundle consecutive stamped edges with the same flags; deltas are u32. */
        unsigned n = 1;
        const sq_item_t *prev = first;
        while (n < q->count && n < UL_EDGES_MAX) {
            const sq_item_t *it = at(q, n);
            if (it->kind != SQ_EDGE || it->flags != first->flags || it->seq != prev->seq + 1u ||
                it->tick <= prev->tick || it->tick - prev->tick > UINT32_MAX) { break; }
            q->pkt.u.edges.dt[n - 1] = (uint32_t)(it->tick - prev->tick);
            prev = it;
            n++;
        }
        q->pkt.kind = UL_KIND_EDGES;
        q->pkt.u.edges.count = (uint8_t)n;
        q->pkt_items = n;
    }
    take_ev_seq(q);
    return &q->pkt;
}

uplink_pl_t *sq_checkpoint(sensor_queue_t *q, uint32_t master_boot_id, uint32_t seq,
                           uint64_t master, uint8_t flags, uint16_t sync_age_ms)
{
    if (!sq_idle(q)) { return NULL; }
    memset(&q->pkt, 0, sizeof(q->pkt));
    q->pkt.kind = UL_KIND_CHECKPOINT;
    q->pkt.flags = flags;
    q->pkt.master_boot_id = master_boot_id;
    q->pkt.capture_seq = seq;
    q->pkt.tick = master;
    q->pkt.sync_age_ms = sync_age_ms;
    q->pkt_items = 0;
    take_ev_seq(q);
    return &q->pkt;
}

void sq_ack(sensor_queue_t *q, uint16_t ack_seq)
{
    if (!q->inflight || ack_seq != q->pkt.ev_seq) { return; }
    q->head = (q->head + q->pkt_items) % SENSOR_FIFO_LEN;
    q->count -= q->pkt_items;
    q->pkt_items = 0;
    q->inflight = 0;
    flush_acc(q);
}
