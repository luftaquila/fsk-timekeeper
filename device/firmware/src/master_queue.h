/* Master host queue. Each record the master accepts becomes one E line
 * with a fresh hseq and its crc, formatted once; the head line is re-sent
 * unchanged every MASTER_USB_RETRY_MS until the host acks exactly that
 * (hseq, master_boot_id, crc). Main-loop context only. */
#ifndef MASTER_QUEUE_H
#define MASTER_QUEUE_H

#include <stdint.h>

#include "config.h"
#include "protocol.h"
#include "proto_usb.h"

typedef struct {
    uint32_t hseq;
    uint32_t master_boot_id;
    uint32_t crc;
    uint32_t last_emit_ms;
    int      emitted;
    char     line[PU_LINE_MAX];
} mq_item_t;

typedef struct {
    mq_item_t item[MASTER_EVENT_QUEUE_LEN];
    unsigned head, count;
    uint32_t next_hseq;      /* 1.. per power-on */
    uint16_t overflow;       /* records refused while full (backpressure) */
    uint32_t ack_mismatch;
} mq_t;

void mq_init(mq_t *q);
int mq_full(const mq_t *q);
unsigned mq_depth(const mq_t *q);

/* Queue one accepted uplink / the master's timebase-end marker. 0 when full. */
int mq_push_uplink(mq_t *q, uint32_t node_id, uint32_t sensor_boot_id, const uplink_pl_t *u,
                   float rssi, float snr);
int mq_push_timebase_end(mq_t *q, uint32_t master_boot_id, uint64_t tick);

/* Host ack. Pops the head on an exact match (1), else counts a mismatch (0). */
int mq_ack(mq_t *q, uint32_t hseq, uint32_t master_boot_id, uint32_t crc);

/* Send or re-send the head line when due. */
void mq_pump(mq_t *q, uint32_t now_ms);

#endif /* MASTER_QUEUE_H */
