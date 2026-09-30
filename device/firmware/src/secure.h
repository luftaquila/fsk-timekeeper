/* Authenticated encryption for the LoRa air link (DESIGN §2.11).
 *
 * Every packet is sealed with XChaCha20-Poly1305 (Monocypher) under a fleet-wide
 * 32-byte key loaded from the flash keystore (never compiled in). Replay
 * resistance: a random per-boot id plus a per-boot counter in the cleartext
 * header, both bound into the nonce.
 *
 * Nonce = (domain | type | node_id | boot_id | ctr): ctr never repeats within a
 * boot and boot_id is fresh per power-up, so a nonce never repeats.
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

/* Reload the key after re-provisioning (no reboot needed). */
void sec_reload(void);

/* 1 when a key is loaded (seal/unseal work), 0 when unprovisioned. */
int sec_provisioned(void);

uint32_t sec_boot_id(void);

/* Non-cryptographic 32-bit random value (MAC slot contention). */
uint32_t sec_random(void);

/* Seal payload into out[] as [header | ciphertext | mac]. node_id is the
 * sender's id (ignored for downlink types, always the master). Returns the wire
 * length, or <0: -1 out too small, -3 counter exhausted, -4 unprovisioned or no
 * boot id. Advances the tx counter. */
int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id,
             const void *payload, int payload_len);

typedef struct {
    uint8_t  type;
    uint32_t node_id;
    uint32_t boot_id;
    uint32_t ctr;
} sec_meta_t;

/* Verify + decrypt. 0 on success; <0: -1 short buffer, -2 MAC failure, -3
 * unprovisioned, -4 protocol-version mismatch. Replay is checked separately. */
int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len);

/* 1 when a received packet of any protocol version authenticates under the
 * fleet key, i.e. a board of this fleet sent it. */
int sec_authentic_any_version(const uint8_t *in, int in_len);

/* Per-(sender, direction) replay window. */
typedef struct {
    uint32_t boot_id;
    uint32_t max_ctr;
    uint8_t  have;
} sec_replay_t;

/* 1 and update st if (boot_id, ctr) is fresh, 0 on replay. A new boot_id
 * re-baselines the window. */
int sec_replay(sec_replay_t *st, uint32_t boot_id, uint32_t ctr);

#ifdef __cplusplus
}
#endif

#endif /* SECURE_H */
