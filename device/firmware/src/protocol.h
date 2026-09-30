/* Sealed LoRa packet formats, radio protocol v11 (DESIGN §2.6, §2.8, §2.11).
 *
 * Wire = [cleartext header | ciphertext payload | 16-byte Poly1305 tag]
 * (secure.h). The type byte carries PROTO_VER in its high nibble, so every
 * packet is version-checked before decryption. The header carries the sender's
 * node_id only on uplinks; the master is the implicit sender of beacons.
 *
 * node_id = low 32 bits of the sender's chip id (never 0); the master is id 0.
 * Every packet has a fixed length.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Low nibble of the type byte. */
#define PKT_TYPE_BEACON   0x01u
#define PKT_TYPE_UPLINK   0x02u

#define PROTO_VER 11u

#define SEC_VT(ver, type) ((uint8_t)(((uint8_t)(ver) << 4) | ((uint8_t)(type) & 0x0Fu)))
#define SEC_VT_TYPE(b)    ((uint8_t)((b) & 0x0Fu))
#define SEC_VT_VER(b)     ((uint8_t)((b) >> 4))

#define SEC_TYPE_HAS_NODE(t) ((t) == PKT_TYPE_UPLINK)

/* Registry capacity at the master = number of uplink slots. */
#define MAX_NODES 5u
#define NODE_MASTER 0u

/* Slot-table name of a sensor id: its low 16 bits, never 0 (0 = free slot).
 * The master refuses to register a second live sensor with the same short id. */
static inline uint16_t node_short_id(uint32_t id)
{
    uint16_t s = (uint16_t)id;
    if (s == 0u) { s = (uint16_t)(id >> 16); }
    return s ? s : 1u;
}

/* ---- Beacon (master -> all) ----------------------------------------------- */

/* Slot k of the table is registry entry k. short_id 0 = free slot. ack_seq is
 * the last ev_seq the master accepted in order from that sensor boot
 * (cumulative ACK); boot_tag = low byte of that boot's id. */
typedef struct __attribute__((packed)) {
    uint16_t short_id;
    uint16_t ack_seq;
    uint8_t  boot_tag;
} beacon_slot_t;

#define BEACON_TX_PREV_VALID 0x01u /* m_tx_prev is the TxDone of beacon seq-1 */

typedef struct __attribute__((packed)) {
    uint8_t  seq;        /* beacon sequence (wraps at 256) */
    uint8_t  flags;      /* BEACON_* */
    uint64_t m_tx_prev;  /* master TxDone tick of beacon seq-1 */
    uint8_t  cp_req;     /* checkpoint request id, 0 = none */
    beacon_slot_t slot[MAX_NODES];
} beacon_pl_t;

/* ---- Uplink (sensor -> master) ------------------------------------------- */

#define UL_KIND_EDGES      1u /* capture_seq..capture_seq+count-1 at tick, tick+dt[0], ... */
#define UL_KIND_LOSS       2u /* capture_seq..end_seq lost between tick and end_tick */
#define UL_KIND_CHECKPOINT 3u /* through tick, the last capture is capture_seq */

#define UL_EDGES_MAX 5u

/* flags: health of the capture clock when the record was stamped. */
#define HEALTH_SYNC_VALID 0x01u
#define HEALTH_SKEW_VALID 0x02u
#define HEALTH_CLOCK_XTAL 0x04u
#define EVENT_INTERPOLATED 0x08u /* stamped by interpolation after a sync gap */
#define EVENT_TIME_UNKNOWN 0x40u /* ticks carry no timing information */
#define HEALTH_EVENT_REQUIRED (HEALTH_SYNC_VALID | HEALTH_SKEW_VALID | HEALTH_CLOCK_XTAL)

typedef struct __attribute__((packed)) {
    uint8_t  health;           /* HEALTH_* of the sensor clock now */
    uint16_t sync_age_ms;      /* age of the newest anchor now, saturated */
    int16_t  skew_ppm;         /* measured drift vs master, clamped to i16 */
    uint16_t rx_miss;          /* beacons missed since boot (saturating) */
    uint8_t  beacon_gap;       /* beacons missed in a row right now (saturating) */
    uint16_t batt_mv;          /* cell estimate */
    int16_t  temp_c10;         /* die temperature, 0.1 C */
    uint16_t capture_overflow; /* ISR ring overflows since boot */
    uint16_t fifo_drop;        /* records discarded (session change) since boot */
    uint16_t err_flags;        /* ERR_* (proto_usb.h), sticky per boot */
    uint8_t  reset_reason;     /* RESET_* (proto_usb.h) */
} ul_diag_t;

typedef struct __attribute__((packed)) {
    uint8_t  kind;           /* UL_KIND_* */
    uint8_t  flags;          /* HEALTH_* | EVENT_* */
    uint16_t ev_seq;         /* transport sequence, 1.. per sensor boot */
    uint32_t master_boot_id; /* master session the ticks belong to */
    uint32_t capture_seq;
    uint64_t tick;           /* master time */
    uint16_t sync_age_ms;    /* anchor age when the first record was stamped */
    union __attribute__((packed)) {
        struct __attribute__((packed)) {
            uint8_t  count;                   /* 1..UL_EDGES_MAX */
            uint32_t dt[UL_EDGES_MAX - 1];    /* tick deltas of edges 2..count */
        } edges;
        struct __attribute__((packed)) {
            uint32_t end_seq;
            uint64_t end_tick;
        } loss;
    } u;
    ul_diag_t diag;
} uplink_pl_t;

/* ---- Wire sizes ---------------------------------------------------------- */

/* Cleartext header: vt(1) + boot_id(4) + ctr(3), plus node_id(4) on uplinks. */
#define SEC_HDR_DL    8
#define SEC_HDR_UL    12
#define SEC_MAC_LEN   16

#define SEC_WIRE_LEN(hdr, pl) ((hdr) + (int)(pl) + SEC_MAC_LEN)

#define WIRE_BEACON   SEC_WIRE_LEN(SEC_HDR_DL, sizeof(beacon_pl_t))  /* 60 B, ~56 ms */
#define WIRE_UPLINK   SEC_WIRE_LEN(SEC_HDR_UL, sizeof(uplink_pl_t))  /* 86 B, ~77 ms */
#define WIRE_MAX      96 /* RX buffer size */

_Static_assert(sizeof(beacon_pl_t) == 36, "beacon payload layout");
_Static_assert(sizeof(uplink_pl_t) == 58, "uplink payload layout");
_Static_assert(WIRE_UPLINK <= WIRE_MAX && WIRE_BEACON <= WIRE_MAX, "packets must fit the RX buffer");

#endif /* PROTOCOL_H */
