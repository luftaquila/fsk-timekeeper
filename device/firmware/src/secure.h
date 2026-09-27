/* Authenticated encryption for the LoRa air link (DESIGN.md §2.11).
 *
 * Every packet is sealed with XChaCha20-Poly1305 (Monocypher) under a fleet-wide
 * 32-byte pre-shared key loaded at boot from the flash keystore (keystore.h) —
 * the key is provisioned per board over the serial 'K' command, never compiled
 * in, so CI builds a key-less app. Confidentiality + integrity + sender
 * authenticity come from the AEAD; replay resistance comes from a per-boot
 * random id plus a monotonic per-boot counter carried in the cleartext header
 * (see protocol.h SEC_HDR_*) and bound into the nonce.
 *
 * Nonce uniqueness (the one rule a stream cipher must never break): the 24-byte
 * nonce is (domain | type | node_id | boot_id | ctr). ctr increments on every
 * seal, so it never repeats within a boot; boot_id is fresh random per power-up,
 * so nonces don't repeat across reboots even though ctr restarts at 0.
 */
#ifndef SECURE_H
#define SECURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seed this node's boot_id (hardware RNG), reset the tx counter, and load the
 * fleet key from the flash keystore. Call once at startup, before any sec_seal(). */
void sec_init(void);

/* Reload the key from the keystore — call after a successful re-provisioning so
 * the new key takes effect without a reboot. */
void sec_reload(void);

/* 1 if a fleet key is present (seal/unseal work), 0 if unprovisioned — the radio
 * stays inert until a key is written via the serial 'K' command. */
int sec_provisioned(void);

uint32_t sec_boot_id(void); /* this node's per-boot random id (for diagnostics) */

/* Seal payload into out[] as [header | ciphertext | mac]. node_id is the
 * SENDER's id (ignored for downlink types, which are always master id 0 and omit
 * it from the wire). Returns the total wire length, or <0: -1 out too small,
 * -3 tx counter exhausted, -4 unprovisioned. Advances this node's tx counter. */
int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id,
             const void *payload, int payload_len);

/* Parsed cleartext header of a received packet. */
typedef struct {
    uint8_t  type;
    uint32_t node_id;
    uint32_t boot_id;
    uint32_t ctr;
} sec_meta_t;

/* Verify + decrypt a received packet of the given payload length. On success
 * fills meta and copies the decrypted payload into out_payload, returns 0.
 * Returns <0: -1 short buffer / length mismatch, -2 MAC failure (forgery / bit
 * error / wrong key), -3 unprovisioned, -4 protocol-version mismatch. Does NOT
 * enforce replay — callers do that with sec_replay once they know which
 * (sender, direction) state to use. */
int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len);

/* Per-(sender, direction) replay window. One per remote talker a receiver
 * tracks: the master keeps one per sensor, a sensor keeps one for the master. */
typedef struct {
    uint32_t boot_id;
    uint32_t max_ctr;
    uint8_t  have;
} sec_replay_t;

/* Returns 1 and updates st if (boot_id, ctr) is fresh; returns 0 on replay.
 * A new boot_id re-baselines the window (a battery sensor that reboots restarts
 * its counter at 0). For the timing-critical EVENT this is backed by a
 * timestamp-freshness gate in main.c so a replayed old event is still rejected. */
int sec_replay(sec_replay_t *st, uint32_t boot_id, uint32_t ctr);

#ifdef __cplusplus
}
#endif

#endif /* SECURE_H */
