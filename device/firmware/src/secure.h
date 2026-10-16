/* Authenticated encryption for the LoRa air link (DESIGN §2.11).
 *
 * XChaCha20-Poly1305 uses purpose-separated control keys and connection keys
 * derived from the provisioned fleet PSK plus a mutual fresh challenge. Only
 * a completed handshake establishes the epoch and increasing receive counter.
 *
 * Nonce = (domain | type | node_id | boot_id | ctr): ctr never repeats within a
 * boot and boot_id is fresh per power-up, making accidental reuse negligible (not impossible).
 */
#ifndef SECURE_H
#define SECURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw a new boot_id from the hardware RNG, reset the tx counter and load the
 * key. Call before any sec_seal(); a master calls it again to open a new session.
 * If the RNG fails, sealing stays refused (a repeated boot_id could repeat a nonce). */
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

/* Nonblocking: consume one precollected 128-bit hardware random challenge.
 * Call sec_service() outside slot-critical work to refill the small pool. */
int sec_challenge(uint8_t out[16]);
int sec_counter_exhausted(void);

typedef struct { uint8_t data[32], confirm[32]; } sec_link_keys_t;
int sec_link_keys(sec_link_keys_t *out, uint64_t master_boot, uint32_t node_id,
                  uint64_t sensor_boot, const uint8_t ns[16], const uint8_t nm[16], uint64_t link_id);
int sec_seal_key(const uint8_t key[32], uint8_t *out, int out_cap, uint8_t type,
                 uint32_t node_id, const void *payload, int payload_len);

/* Seal payload into out[] as [header | ciphertext | mac]; node_id is the sender's (ignored for
 * downlink, always the master). Returns the wire length, or -1 out too small, -3 counter
 * exhausted, -4 unprovisioned or no boot id. Advances the tx counter. */
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

/* Verify + decrypt. 0 on success; <0: -1 short buffer, -2 MAC failure, -3
 * unprovisioned, -4 protocol-version mismatch. Replay is checked separately. */
int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len);

/* 1 when a received packet of any protocol version authenticates under the
 * fleet key, i.e. a board of this fleet sent it. */
int sec_authentic_any_version(const uint8_t *in, int in_len);

/* Counter for one explicitly authenticated connection. Changing a packet's
 * boot ID never resets this state; only a completed handshake may replace it. */
typedef struct { uint32_t max_ctr; } sec_rx_counter_t;

#ifdef __cplusplus
}
#endif

#endif /* SECURE_H */
