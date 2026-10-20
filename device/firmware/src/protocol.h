/* Sealed LoRa packet formats, radio protocol v17 (DESIGN §2.6, §2.8, §2.11).
 *
 * Wire = [cleartext header | ciphertext payload | 16-byte Poly1305 tag]
 * (secure.h). The first byte carries both version and type, so every
 * packet is version-checked before decryption. The header carries the sender's
 * node_id only on uplinks; the master is the implicit sender of beacons.
 *
 * node_id = low 32 bits of the sender's chip id (never 0); the master is id 0.
 * Every packet has a fixed length.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <assert.h>
#include <stdint.h>
#include "radio_health.h"

/* Low three bits of the first byte. */
#define PKT_TYPE_BEACON   0x01u
#define PKT_TYPE_UPLINK   0x02u
#define PKT_TYPE_JOIN     0x03u
#define PKT_TYPE_OFFER    0x04u
#define PKT_TYPE_CONFIRM  0x05u

/* v16 compacted fields; v17 adds authenticated equal-length two-way samples.
 * Preserve all v1..v16 byte encodings (high nibble 0 means v16). Previously
 * unused type bit 3 extends the version to v17..v32 without another wire byte.
 * Only packet types 1..5 have ever been assigned. */
#define PROTO_VER 17u

#define SEC_VT(ver, type) ((uint8_t)(((uint8_t)(ver) << 4) | \
                         ((((uint8_t)(ver) - 1u) & 0x10u) >> 1) | ((uint8_t)(type) & 0x07u)))
#define SEC_VT_TYPE(b)    ((uint8_t)((b) & 0x07u))
#define SEC_VT_VER(b)     ((uint8_t)(((((uint8_t)(b) >> 4) + 15u) & 0x0fu) + 1u + (((b) & 0x08u) << 1)))

#define SEC_TYPE_HAS_NODE(t) ((t) == PKT_TYPE_UPLINK || (t) == PKT_TYPE_JOIN || (t) == PKT_TYPE_CONFIRM)

/* Registry capacity at the master = entries of the beacon's slot table. */
#define MAX_NODES 5u
#define NODE_MASTER 0u

/* ---- Beacon (master -> all) ----------------------------------------------- */

/* Entry k names the complete sender and negotiated connection. node_id 0 = free entry.
 * ack_seq is the last evidence packet accepted from that sensor boot, acknowledged only on its active connection. */
typedef struct __attribute__((packed)) {
    uint32_t node_id;
    uint16_t ack_seq;
    uint32_t link_id;
} beacon_slot_t;

#define BEACON_TX_PREV_VALID 0x01u /* m_tx_prev is the TxDone of beacon seq-1 */
#define BEACON_ADMISSION     0x02u
#define BEACON_RX_PREV_VALID 0x04u /* m_rx_prev is a matching uplink of beacon seq-1 */

typedef struct __attribute__((packed)) {
    uint8_t  seq;        /* beacon sequence (wraps at 256) */
    uint8_t  flags;      /* BEACON_* */
    uint64_t m_tx_prev;  /* master TxDone tick of beacon seq-1 */
    uint8_t  cp_req;     /* checkpoint request id, 0 = none */
    beacon_slot_t slot[MAX_NODES];
    uint64_t m_rx_prev;  /* master's RxDone of the selected sensor's previous-frame uplink */
    uint8_t  rx_entry;   /* registry entry of that sensor; valid only with RX_PREV_VALID */
} beacon_pl_t;

/* Six explicit slot roles while admission is open. Low three bits name the
 * registry entry; the upper bits are the role. Unused slots must be zero. */
#define SLOT_ROLE_IDLE     0x00u
#define SLOT_ROLE_DATA     0x08u
#define SLOT_ROLE_REQUEST  0x10u
#define SLOT_ROLE_OFFER    0x18u
#define SLOT_ROLE_CONFIRM  0x20u
#define SLOT_ROLE(v) ((uint8_t)((v) & 0xf8u))
#define SLOT_ENTRY(v) ((unsigned)((v) & 7u))
typedef struct __attribute__((packed)) {
    beacon_pl_t base;
    uint8_t schedule[6];
    uint8_t pending;
} admission_beacon_t;

typedef struct __attribute__((packed)) {
    uint64_t master_boot;
    uint8_t ns[16];
} join_pl_t;
typedef struct __attribute__((packed)) {
    uint32_t node_id;
    uint64_t sensor_boot;
    uint8_t ns[16], nm[16];
    uint32_t link_id;
    uint8_t beacon_ctr[3];
} offer_pl_t;
typedef struct __attribute__((packed)) {
    uint64_t master_boot;
    uint8_t nm[16];
} confirm_pl_t;

/* ---- Uplink (sensor -> master) ------------------------------------------- */

#define UL_KIND_SYNC       0u /* diagnostics/two-way probe only; never advances the evidence ACK */
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
    uint16_t capture_overflow; /* lost/ambiguous hardware captures since boot */
    uint16_t fifo_drop;        /* records discarded (session change) since boot */
    uint16_t err_flags;        /* active radio faults; non-radio bits sticky per boot */
    uint8_t  reset_reason;     /* RESET_* (proto_usb.h) */
} ul_diag_t;

typedef struct __attribute__((packed)) {
    uint8_t  kind;           /* UL_KIND_* */
    uint8_t  flags;          /* HEALTH_* | EVENT_* */
    uint16_t ev_seq;         /* transport sequence, 1.. per sensor boot */
    uint64_t master_boot_id; /* master session the ticks belong to */
    uint32_t capture_seq;
    uint64_t tick;           /* master time */
    uint16_t sync_age_ms;    /* anchor age when the first record was stamped */
    union __attribute__((packed)) {
        radio_history_t history; /* CHECKPOINT only; frozen until acknowledged */
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
    uint32_t beacon_ctr; /* current beacon's authenticated 24-bit counter; refreshed on retries */
} uplink_pl_t;

/* ---- Wire sizes ---------------------------------------------------------- */

/* Models above retain 64-bit arithmetic for ticks and unpacked fields for MAC
 * users. Only the authenticated radio payload is compacted. At 16 MHz, 48 bits
 * cover 203.6 days; supported uptime is at most one day, with no epoch extension.
 * Random boot ids are NOT ticks and retain all 64 bits. */
#define PL_BEACON     66
#define PL_ADMISSION  71
#define PL_UPLINK     62
#define PL_JOIN       24
#define PL_OFFER      51
#define PL_CONFIRM    24

/* Return encoded payload size, or -1 for an unknown type/model size. */
int protocol_payload_size(uint8_t type, int model_len);
/* Encode/decode the model, with explicit lengths; -1 rejects invalid packing.
 * Decode leaves the output untouched on failure. Encode returns bytes written;
 * decode returns 0. Callers must authenticate before decoding received bytes. */
int protocol_encode(uint8_t *out, int cap, uint8_t type, const void *model, int model_len);
int protocol_decode(void *model, int model_len, uint8_t type, const uint8_t *in, int len);

/* Cleartext header: vt(1) + boot_id(8) + ctr(3), plus node_id(4) on uplinks. */
#define SEC_HDR_DL    12
#define SEC_HDR_UL    16
#define SEC_MAC_LEN   16

#define SEC_WIRE_LEN(hdr, pl) ((hdr) + (int)(pl) + SEC_MAC_LEN)

#define WIRE_BEACON    SEC_WIRE_LEN(SEC_HDR_DL, PL_BEACON)
#define WIRE_UPLINK    SEC_WIRE_LEN(SEC_HDR_UL, PL_UPLINK)
#define WIRE_ADMISSION SEC_WIRE_LEN(SEC_HDR_DL, PL_ADMISSION)
/* Admission uplinks append five authenticated zero bytes, matching that beacon. */
#define WIRE_UPLINK_ADMISSION WIRE_ADMISSION
#define WIRE_JOIN      SEC_WIRE_LEN(SEC_HDR_UL, PL_JOIN)
#define WIRE_OFFER     SEC_WIRE_LEN(SEC_HDR_DL, PL_OFFER)
#define WIRE_CONFIRM   SEC_WIRE_LEN(SEC_HDR_UL, PL_CONFIRM)
#define WIRE_MAX      128 /* RX buffer size */

static_assert(sizeof(radio_history_t) == 17, "checkpoint history layout");
static_assert(PROTO_VER >= 1 && PROTO_VER <= 32, "version field exhausted");
static_assert(PKT_TYPE_CONFIRM <= 7, "packet types must fit three bits");
static_assert(sizeof(beacon_slot_t) == 10 && sizeof(beacon_pl_t) == 70, "beacon model layout");
static_assert(sizeof(uplink_pl_t) == 66, "uplink model layout");
static_assert(sizeof(join_pl_t) == PL_JOIN && sizeof(offer_pl_t) == PL_OFFER && sizeof(confirm_pl_t) == PL_CONFIRM,
              "unpacked admission payload layout");
static_assert(WIRE_BEACON == 94 && WIRE_UPLINK == WIRE_BEACON && WIRE_ADMISSION == 99 &&
              WIRE_JOIN == 56 && WIRE_OFFER == 79 && WIRE_CONFIRM == 56,
              "admission airtime contract");
static_assert(WIRE_UPLINK <= WIRE_MAX && WIRE_BEACON <= WIRE_MAX, "packets must fit the RX buffer");

#endif /* PROTOCOL_H */
