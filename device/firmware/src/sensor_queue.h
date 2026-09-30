/* Sensor evidence FIFO (DESIGN §2.8, W2). Pure logic.
 *
 * Records enter in capture-seq order and leave only when the master's
 * cumulative ACK covers the packet that carried them, so nothing expires. When
 * the FIFO is full, new records merge into one loss range that enters as soon
 * as there is room (seq order is kept: while it waits, later records join it).
 * An edge captured while sync was stale is held in local ticks and stamped when
 * the next anchor arrives, or becomes a loss when that is impossible.
 */
#ifndef SENSOR_QUEUE_H
#define SENSOR_QUEUE_H

#include <stdint.h>

#include "config.h"
#include "protocol.h"
#include "sync.h"

#define SQ_EDGE 1u /* stamped capture: tick = master time */
#define SQ_HELD 2u /* capture awaiting a stamp: tick = local time */
#define SQ_LOSS 3u /* seq..end_seq lost; ticks in master time unless EVENT_TIME_UNKNOWN */

typedef struct {
    uint8_t  kind;
    uint8_t  flags;       /* HEALTH_* | EVENT_* */
    uint16_t sync_age_ms;
    uint32_t seq;
    uint32_t end_seq;
    uint64_t tick;
    uint64_t end_tick;
} sq_item_t;

typedef struct {
    sq_item_t item[SENSOR_FIFO_LEN];
    unsigned head, count;
    int acc_pending;     /* loss range waiting for room */
    sq_item_t acc;
    int inflight;        /* pkt is on air until its ev_seq is acked */
    uplink_pl_t pkt;
    unsigned pkt_items;  /* FIFO records carried by pkt (0 = checkpoint) */
    uint16_t next_ev_seq;
    uint16_t fifo_drop;  /* records discarded by sq_clear() */
} sensor_queue_t;

void sq_init(sensor_queue_t *q);

/* New master session: every record is bound to the old timebase. */
void sq_clear(sensor_queue_t *q);

/* Add one capture: stamped (SQ_EDGE, master tick), held (SQ_HELD, local tick) or
 * a single-seq loss of unknown time. */
void sq_push_edge(sensor_queue_t *q, uint32_t seq, uint64_t master, uint8_t flags, uint16_t sync_age_ms);
void sq_push_held(sensor_queue_t *q, uint32_t seq, uint64_t local);
void sq_push_loss(sensor_queue_t *q, uint32_t first_seq, uint32_t last_seq,
                  uint64_t first_tick, uint64_t last_tick, uint8_t flags);

/* Re-stamp held captures after a new anchor; those that can never be stamped
 * (or whose anchor aged past SYNC_HOLD_MAX_MS by now_local) become losses. */
void sq_resolve(sensor_queue_t *q, const sync_t *s, uint64_t now_local);

/* 1 when nothing is queued, accumulating or on air. */
int sq_idle(const sensor_queue_t *q);

/* Packet for this slot: the in-flight one, else built from the FIFO head
 * (up to UL_EDGES_MAX consecutive stamped edges with equal flags, or one loss).
 * Returns NULL when the head is still held or nothing is queued. */
uplink_pl_t *sq_packet(sensor_queue_t *q, uint32_t master_boot_id);

/* Make a checkpoint the in-flight packet (only when sq_idle()). */
uplink_pl_t *sq_checkpoint(sensor_queue_t *q, uint32_t master_boot_id, uint32_t seq,
                           uint64_t master, uint8_t flags, uint16_t sync_age_ms);

/* Beacon ACK for this sensor boot: drops the in-flight packet when ack_seq is its ev_seq. */
void sq_ack(sensor_queue_t *q, uint16_t ack_seq);

#endif /* SENSOR_QUEUE_H */
