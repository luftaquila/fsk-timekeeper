#include "secure.h"

#include <string.h>

#include "protocol.h"
#include "keystore.h"
#include "monocypher.h"
#include "nrf.h"

/* Nonce domain byte — separates this protocol's use of the key from any other.
 * 'W' for FSK-WL (wireless). */
#define NONCE_DOMAIN 0x57u

/* Fleet-wide pre-shared key, loaded at boot from the flash keystore (NOT
 * compiled in — see keystore.h / DESIGN §2.11). g_provisioned is 0 until a key
 * is present; seal/unseal then refuse, so an unprovisioned board is inert on
 * the air. */
static uint8_t  KEY[KEYSTORE_KEY_LEN];
static int      g_provisioned;
static uint32_t g_boot_id;
static uint32_t g_tx_ctr;

/* Blocking 32-bit read of the nRF52840 hardware RNG (bias-corrected). Used once
 * at boot for boot_id; latency is irrelevant. */
static uint32_t rng32(void)
{
    NRF_RNG->CONFIG = (RNG_CONFIG_DERCEN_Enabled << RNG_CONFIG_DERCEN_Pos);
    NRF_RNG->TASKS_START = 1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        NRF_RNG->EVENTS_VALRDY = 0;
        while (NRF_RNG->EVENTS_VALRDY == 0) { /* wait for a fresh byte */ }
        v = (v << 8) | (NRF_RNG->VALUE & 0xFFu);
    }
    NRF_RNG->TASKS_STOP = 1;
    return v;
}

void sec_init(void)
{
    g_boot_id = rng32();
    g_tx_ctr = 0;
    sec_reload();
}

void sec_reload(void)
{
    g_provisioned = keystore_load(KEY);
}

int sec_provisioned(void) { return g_provisioned; }

uint32_t sec_boot_id(void) { return g_boot_id; }

/* 24-byte XChaCha nonce from the message identity. Unique per (key, message):
 * ctr never repeats within a boot, boot_id makes it unique across boots. */
static void build_nonce(uint8_t nonce[24], uint8_t type, uint32_t node_id,
                        uint32_t boot_id, uint32_t ctr)
{
    memset(nonce, 0, 24);
    nonce[0] = NONCE_DOMAIN;
    nonce[1] = type;
    memcpy(&nonce[2], &node_id, 4);
    memcpy(&nonce[6], &boot_id, 4);
    memcpy(&nonce[10], &ctr, 4);
}

int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id,
             const void *payload, int payload_len)
{
    if (!g_provisioned) { return -4; } /* no key — refuse to transmit */
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (out_cap < wire) { return -1; }

    /* Never let ctr wrap: a repeated (boot_id, ctr) would reuse a nonce, which is
     * catastrophic for the stream cipher. 2^24 seals is unreachable in a session
     * (194 days at 1 Hz); refuse rather than wrap. */
    if (g_tx_ctr >= 0xFFFFFFu) { return -3; }
    uint32_t ctr = ++g_tx_ctr; /* first sealed packet uses ctr = 1 */

    /* Downlink packets are always sent by the master (id 0): both sides feed
     * NODE_MASTER to the nonce and the id is omitted from the wire. */
    uint32_t nid = has_node ? node_id : NODE_MASTER;

    /* Serialize the cleartext header — this is also the AEAD associated data, so
     * any tamper of vt/boot_id/ctr/node_id fails the tag. Little-endian; the
     * 24-bit ctr's implicit high byte is 0 (matched in build_nonce). */
    uint8_t *p = out;
    *p++ = SEC_VT(PROTO_VER, type);
    memcpy(p, &g_boot_id, 4); p += 4;
    *p++ = (uint8_t)(ctr & 0xFFu);
    *p++ = (uint8_t)((ctr >> 8) & 0xFFu);
    *p++ = (uint8_t)((ctr >> 16) & 0xFFu);
    if (has_node) { memcpy(p, &nid, 4); p += 4; }

    uint8_t nonce[24];
    build_nonce(nonce, type, nid, g_boot_id, ctr);

    crypto_aead_lock(out + hdr,               /* ciphertext */
                     out + hdr + payload_len, /* mac (16) */
                     KEY, nonce,
                     out, (size_t)hdr,        /* associated data = header */
                     (const uint8_t *)payload, (size_t)payload_len);
    return wire;
}

int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len)
{
    if (!g_provisioned) { return -3; } /* no key — can't authenticate anything */
    if (in_len < 1) { return -1; }

    uint8_t vt = in[0];
    if (SEC_VT_VER(vt) != PROTO_VER) { return -4; } /* foreign / old protocol version */
    uint8_t type = SEC_VT_TYPE(vt);
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (in_len < wire) { return -1; }

    const uint8_t *p = in + 1;
    uint32_t boot_id; memcpy(&boot_id, p, 4); p += 4;
    uint32_t ctr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); p += 3;
    uint32_t nid = NODE_MASTER;
    if (has_node) { memcpy(&nid, p, 4); p += 4; }

    uint8_t nonce[24];
    build_nonce(nonce, type, nid, boot_id, ctr);

    uint8_t plain[WIRE_MAX];
    if (crypto_aead_unlock(plain,
                           in + hdr + payload_len, /* mac */
                           KEY, nonce,
                           in, (size_t)hdr,        /* associated data */
                           in + hdr, (size_t)payload_len) != 0) {
        return -2; /* forgery / bit error / wrong key */
    }

    memcpy(out_payload, plain, (size_t)payload_len);
    crypto_wipe(plain, sizeof(plain));

    meta->type = type;
    meta->node_id = nid;
    meta->boot_id = boot_id;
    meta->ctr = ctr;
    return 0;
}

int sec_replay(sec_replay_t *st, uint32_t boot_id, uint32_t ctr)
{
    if (!st->have || boot_id != st->boot_id) {
        st->have = 1;
        st->boot_id = boot_id;
        st->max_ctr = ctr;
        return 1; /* first contact or a new boot session */
    }
    if (ctr > st->max_ctr) {
        st->max_ctr = ctr;
        return 1;
    }
    return 0; /* stale: replayed or out-of-order */
}
