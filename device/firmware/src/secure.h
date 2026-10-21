/* Authenticated encryption for the LoRa link: XChaCha20-Poly1305 with purpose-separated control
 * keys and connection keys from the fleet PSK plus a mutual fresh challenge; only a completed
 * handshake sets the epoch and the increasing receive counter.
 * Nonce = (domain | type | node_id | boot_id | ctr): ctr never repeats within a boot and boot_id
 * is fresh per power-up, so accidental reuse is negligible. */
#ifndef SECURE_H
#define SECURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* New RNG boot_id, tx counter reset, key load; before any sec_seal() and per master session. On
 * RNG failure no sealing until sec_service() draws one (each second); reuse could repeat nonces. */
void sec_init(void);
void sec_service(uint32_t now_ms);
int sec_boot_ready(void);
int sec_ready(void);

/* Reload the PSK; the caller must invalidate existing connection keys. */
void sec_reload(void);

/* 1 when a key is loaded; transmission also requires sec_ready(). */
int sec_provisioned(void);

uint64_t sec_boot_id(void);

/* Non-cryptographic 32-bit random value (MAC slot contention). */
uint32_t sec_random(void);

/* Nonblocking: take one precollected 128-bit RNG challenge; sec_service() refills the pool. */
int sec_challenge(uint8_t out[16]);
int sec_counter_exhausted(void);

typedef struct { uint8_t data[32], confirm[32]; } sec_link_keys_t;
int sec_link_keys(sec_link_keys_t *out, uint64_t master_boot, uint32_t node_id,
                  uint64_t sensor_boot, const uint8_t ns[16], const uint8_t nm[16], uint32_t link_id);
int sec_seal_key(const uint8_t key[32], uint8_t *out, int out_cap, uint8_t type,
                 uint32_t node_id, const void *payload, int payload_len);
/* Uplink: pad to the received beacon's wire length; the padding is encrypted and must be zero. */
int sec_seal_uplink(const uint8_t key[32], uint8_t *out, int out_cap, uint32_t node_id,
                     const void *payload, int payload_len, int wire_len);

/* Seal a payload model (payload_len = its sizeof) as [header|ciphertext|mac], advancing the tx
 * counter; node_id = sender (ignored downlink). Returns the wire length, or -1 out too small or
 * payload not encodable, -3 counter exhausted, -4 no key/boot id, -5 not a control type. */
int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id,
             const void *payload, int payload_len);

typedef struct {
    uint8_t  type;
    uint32_t node_id;
    uint64_t boot_id;
    uint32_t ctr;
} sec_meta_t;
int sec_unseal_key(const uint8_t key[32], const uint8_t *in, int in_len, sec_meta_t *meta,
                   void *out_payload, int payload_len);
uint32_t sec_wire_counter(const uint8_t *wire);
/* A pinned connection never learns an epoch from an unsolicited packet. */
int sec_accept_counter(uint32_t *high, uint32_t ctr);

/* Verify + decrypt: 0, or -1 malformed (length, type, zero counter, padding, payload), -2 MAC
 * failure, -3 unprovisioned, -4 protocol-version mismatch. Replay is checked separately. */
int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len);

/* Diagnostics only: 1 if a control key authenticates it; DATA/CONFIRM need link keys. */
int sec_authentic_any_version(const uint8_t *in, int in_len);

/* Per authenticated connection; a packet's boot ID never resets it, only a completed handshake. */
typedef struct { uint32_t max_ctr; } sec_rx_counter_t;

#ifdef __cplusplus
}
#endif

#endif /* SECURE_H */
