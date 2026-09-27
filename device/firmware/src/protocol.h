/* Secured LoRa packet formats + time-sync helpers (DESIGN.md §2.5, §2.6, §2.11).
 *
 * One master + up to 6 sensors, single channel, single timebase (the master's
 * 16 MHz TIMER1). node_id is the SENDER's own identity = the low 32 bits of its
 * chip id (FICR.DEVICEID); the master is the reserved id 0. Nothing is hardcoded
 * and there is no slot/number: a sensor transmits under its own id and the master
 * auto-registers it on first contact, reporting that id over USB. The
 * sensor->event mapping lives on the server, so packets carry no set_id.
 *
 * SECURITY (DESIGN §2.11): every air packet is sealed with XChaCha20-Poly1305
 * (Monocypher) under a fleet-wide pre-shared key. Wire layout is
 *
 *     [ sec header (cleartext, authenticated) | ciphertext(payload) | mac(16) ]
 *
 * The cleartext header carries the sender id + the anti-replay counters and is
 * fed to the AEAD as associated data (so it cannot be tampered). The payload
 * struct (timestamps, offsets, ...) is encrypted. The 16-byte Poly1305 tag
 * detects both bit errors and forgery. See secure.h.
 *
 * WIRE COMPACTNESS: the header is variable-length and the payloads are
 * right-sized to keep airtime down on the one shared LoRa channel:
 *   - The protocol version rides in the high nibble of the type byte (SEC_VT_*),
 *     so every packet is version-checked with no extra byte.
 *   - node_id is serialized ONLY on uplink packets (EVENT/STATUS); downlink
 *     packets (BEACON/ACK) are always sent by the master (id 0), which both
 *     sides supply implicitly to the nonce — see secure.c.
 *   - ctr is 24-bit on the wire (16.7M seals/session = 194 days at 1 Hz).
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Low nibble of the type byte. Values 1..15; the high nibble carries PROTO_VER. */
#define PKT_TYPE_BEACON   0x01u
#define PKT_TYPE_EVENT    0x02u
#define PKT_TYPE_ACK      0x03u
#define PKT_TYPE_STATUS   0x04u

#define PROTO_VER   9u  /* source sequence, loss ranges, and reliable checkpoints */

/* Type byte = (PROTO_VER << 4) | PKT_TYPE_*. Both nibbles are authenticated as
 * AEAD associated data and the type half feeds the nonce. PROTO_VER must stay <= 15. */
#define SEC_VT(ver, type) ((uint8_t)(((uint8_t)(ver) << 4) | ((uint8_t)(type) & 0x0Fu)))
#define SEC_VT_TYPE(b)    ((uint8_t)((b) & 0x0Fu))
#define SEC_VT_VER(b)     ((uint8_t)((b) >> 4))

/* Uplink packets (sensor -> master) carry the sender's 32-bit node_id in the
 * cleartext header so the master can demux + build the nonce. Downlink packets
 * (master -> sensor) omit it: the sender is always the master (id 0). */
#define SEC_TYPE_HAS_NODE(t) ((t) == PKT_TYPE_EVENT || (t) == PKT_TYPE_STATUS)

/* node_id is a 32-bit identity: the master is the reserved id 0 (NODE_MASTER), a
 * sensor is the low 32 bits of its own chip id (never 0 — see node_sender_id()).
 * The master tracks sensors in a small registry (MAX_NODES entries) keyed by id,
 * auto-registering on first contact; entries are a set, not indexed by id. */
#define MAX_NODES   7u  /* max sensors tracked at once (registry capacity) */
#define NODE_MASTER 0u

/* ---- Encrypted payloads (plaintext form, before sealing) ----------------- */

/* Master -> all sensors (broadcast sync anchor). Carries the master TxDone tick
 * of the PREVIOUS beacon so a sensor can pair it with its own stored RxDone of
 * that beacon (§2.5). The beacon/sync period and STATUS cycle are fixed config
 * constants shared by both roles (config.h), so they are NOT carried on air. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;        /* beacon sequence (wraps at 256) */
    uint64_t m_tx_prev;  /* master TxDone tick of beacon (seq-1) */
} beacon_pl_t;

/* Sensor -> master. ev_master_t = event tick already mapped to master time.
 * master_boot_id = the complete master session boot_id this event is
 * synced to (from the beacons the sensor tracks). The master rejects events that
 * don't name its current session, so an event captured under a previous master
 * power-cycle cannot be replayed after the master reboots (DESIGN §2.11).
 * Preserve the complete boot ID; truncation would weaken the session fence. */
typedef struct __attribute__((packed)) {
    uint16_t ev_seq;
    uint64_t ev_master_t;
    uint32_t master_boot_id; /* complete master boot_id (session binding) */
    uint16_t sync_age_ms;    /* age of the offset anchor when the edge was captured */
    uint8_t  flags;
    uint32_t capture_seq;
    uint32_t end_seq;
    uint64_t end_tick;
} event_pl_t;

#define EVENT_LOSS 0x10u
#define EVENT_CHECKPOINT 0x20u
#define EVENT_TIME_UNKNOWN 0x40u

#define HEALTH_SYNC_VALID 0x01u
#define HEALTH_SKEW_VALID 0x02u
#define HEALTH_CLOCK_XTAL 0x04u
#define HEALTH_CAPTURE_OK 0x08u
#define HEALTH_EVENT_REQUIRED (HEALTH_SYNC_VALID | HEALTH_SKEW_VALID | HEALTH_CLOCK_XTAL | HEALTH_CAPTURE_OK)

/* Master -> sensor: acknowledges a received EVENT so the sensor stops
 * retransmitting (DESIGN §2.8). The header node_id is the master (0, implicit);
 * the acked sensor is named here in the payload by its 32-bit id. */
typedef struct __attribute__((packed)) {
    uint32_t node_id;  /* sensor being acked (its low-32 chip id) */
    uint16_t ev_seq;   /* event being acked */
    uint32_t sensor_boot_id;
    uint64_t ev_master_t;
} ack_pl_t;

/* Sensor -> master: periodic diagnostics (DESIGN §2.10), sent once per STATUS
 * cycle at a chip-id-hashed phase within the synced cycle (collision-resistant
 * without coordination, §2.8). offset/skew are the sensor's own sync health; the
 * master adds RSSI/SNR/last-seen/latency on its side. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;         /* status sequence (uplink-loss detect) */
    int64_t  offset_tick; /* current master-time offset (master_t - local_t) */
    int16_t  skew_ppm;    /* clock drift estimate, signed ppm (+-32767 covers any real XO) */
    uint16_t rx_miss;     /* beacons missed since boot (saturating) */
    uint8_t  beacon_gap;  /* consecutive beacons missed right now (saturating at 255) */
    uint16_t batt_mv;     /* cell estimate (VDDH via SAADC VDDHDIV5 + diode drop) */
    int16_t  temp_c10;    /* nRF die temperature, deci-degrees C (235 = 23.5 C) */
    uint16_t sync_age_ms; /* current offset-anchor age, saturated */
    uint16_t capture_overflow; /* sticky SENSOR ISR ring overflow count */
    uint16_t event_drop;  /* post-sync events rejected, overflowed, or expired before ACK */
    uint8_t  flags;       /* HEALTH_* bits */
} status_pl_t;

/* ---- Wire sizes ---------------------------------------------------------- */

/* Cleartext header: vt(1) + boot_id(4) + ctr(3), plus node_id(4) on uplink. */
#define SEC_HDR_DL    8                        /* downlink (BEACON/ACK): no node_id */
#define SEC_HDR_UL    12                       /* uplink (EVENT/STATUS): + node_id */
#define SEC_MAC_LEN   16                       /* Poly1305 tag */

/* Sealed wire length for a given header length + plaintext payload length. */
#define SEC_WIRE_LEN(hdr, pl) ((hdr) + (int)(pl) + SEC_MAC_LEN)

#define WIRE_BEACON   SEC_WIRE_LEN(SEC_HDR_DL, sizeof(beacon_pl_t))  /* 33 */
#define WIRE_EVENT    SEC_WIRE_LEN(SEC_HDR_UL, sizeof(event_pl_t))
#define WIRE_ACK      SEC_WIRE_LEN(SEC_HDR_DL, sizeof(ack_pl_t))
#define WIRE_STATUS   SEC_WIRE_LEN(SEC_HDR_UL, sizeof(status_pl_t))
#define WIRE_MAX      64 /* RX buffer size; largest sealed packet is WIRE_EVENT */

_Static_assert(WIRE_EVENT <= WIRE_MAX, "EVENT must fit the radio frame");
_Static_assert(WIRE_STATUS <= WIRE_MAX, "STATUS must fit the radio frame");

#endif /* PROTOCOL_H */
