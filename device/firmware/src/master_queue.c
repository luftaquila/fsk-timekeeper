#include "master_queue.h"

#include <string.h>

#include "errlog.h"
#include "usb.h"

void mq_init(mq_t *q)
{
    memset(q, 0, sizeof(*q));
    q->next_hseq = 1;
}

int mq_full(const mq_t *q)
{
    return q->count >= MASTER_EVENT_QUEUE_LEN;
}

unsigned mq_depth(const mq_t *q)
{
    return q->count;
}

static mq_item_t *tail_slot(mq_t *q)
{
    if (mq_full(q)) {
        if (q->overflow != UINT16_MAX) { q->overflow++; }
        el_note(EL_QUEUE_FULL);
        return NULL;
    }
    return &q->item[(q->head + q->count) % MASTER_EVENT_QUEUE_LEN];
}

static int commit(mq_t *q, mq_item_t *it, uint32_t hseq, uint32_t master_boot_id, int len)
{
    if (len < 0) { return 0; } /* cannot happen: PU_LINE_MAX holds the longest line */
    it->hseq = hseq;
    it->master_boot_id = master_boot_id;
    it->emitted = 0;
    q->count++;
    if (++q->next_hseq == 0) { q->next_hseq = 1; }
    return 1;
}

int mq_push_uplink(mq_t *q, uint32_t node_id, uint32_t sensor_boot_id, const uplink_pl_t *u,
                   float rssi, float snr)
{
    mq_item_t *it = tail_slot(q);
    if (!it) { return 0; }
    uint32_t hseq = q->next_hseq;
    int len = pu_format_uplink(it->line, sizeof(it->line), hseq, node_id, sensor_boot_id, u, rssi, snr, &it->crc);
    return commit(q, it, hseq, u->master_boot_id, len);
}

int mq_push_timebase_end(mq_t *q, uint32_t master_boot_id, uint64_t tick)
{
    mq_item_t *it = tail_slot(q);
    if (!it) { return 0; }
    uint32_t hseq = q->next_hseq;
    int len = pu_format_timebase_end(it->line, sizeof(it->line), hseq, master_boot_id, tick, &it->crc);
    return commit(q, it, hseq, master_boot_id, len);
}

int mq_ack(mq_t *q, uint32_t hseq, uint32_t master_boot_id, uint32_t crc)
{
    /* A repeated ack of a line already popped (the host saw a re-send) is
     * harmless and ignored; anything else that does not match is counted. */
    if (!q->count) { return 0; }
    const mq_item_t *h = &q->item[q->head];
    if ((int32_t)(hseq - h->hseq) < 0) { return 0; }
    if (h->hseq == hseq && h->master_boot_id == master_boot_id && h->crc == crc) {
        q->head = (q->head + 1u) % MASTER_EVENT_QUEUE_LEN;
        q->count--;
        return 1;
    }
    q->ack_mismatch++;
    el_note(EL_ACK_MISMATCH);
    return 0;
}

void mq_pump(mq_t *q, uint32_t now_ms)
{
    if (!q->count) { return; }
    mq_item_t *h = &q->item[q->head];
    if (h->emitted && (uint32_t)(now_ms - h->last_emit_ms) < MASTER_USB_RETRY_MS) { return; }
    if (usb_write(h->line)) {
        h->emitted = 1;
        h->last_emit_ms = now_ms;
    }
}
