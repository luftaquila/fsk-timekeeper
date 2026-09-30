#include "secure.h"

#include <string.h>

#include "board.h"
#include "errlog.h"
#include "protocol.h"
#include "keystore.h"
#include "monocypher.h"
#include "nrf.h"

/* Nonce domain byte ('W' for FSK-WL) separates this use of the key. */
#define NONCE_DOMAIN 0x57u

/* A bias-corrected RNG byte takes ~100 us. */
#define RNG_BYTE_MAX_US 5000u

static uint8_t  KEY[KEYSTORE_KEY_LEN];
static int      g_provisioned;
static int      g_have_boot_id;
static uint32_t g_boot_id;
static uint32_t g_tx_ctr;
static uint32_t g_prng;

static int rng32(uint32_t *out)
{
    NRF_RNG->CONFIG = (RNG_CONFIG_DERCEN_Enabled << RNG_CONFIG_DERCEN_Pos);
    NRF_RNG->TASKS_START = 1;
    uint32_t v = 0;
    int ok = 1;
    for (int i = 0; i < 4 && ok; i++) {
        NRF_RNG->EVENTS_VALRDY = 0;
        uint32_t t0 = board_micros();
        while (NRF_RNG->EVENTS_VALRDY == 0) {
            if ((uint32_t)(board_micros() - t0) >= RNG_BYTE_MAX_US) { ok = 0; break; }
        }
        v = (v << 8) | (NRF_RNG->VALUE & 0xFFu);
    }
    NRF_RNG->TASKS_STOP = 1;
    if (!ok) { el_note(EL_HW_TIMEOUT); return 0; }
    *out = v;
    return 1;
}

void sec_init(void)
{
    g_have_boot_id = rng32(&g_boot_id);
    g_tx_ctr = 0;
    g_prng = g_boot_id ^ board_micros() ^ 0x9E3779B9u;
    sec_reload();
}

void sec_reload(void)
{
    g_provisioned = keystore_load(KEY);
}

int sec_provisioned(void) { return g_provisioned; }

uint32_t sec_boot_id(void) { return g_boot_id; }

uint32_t sec_random(void)
{
    /* xorshift32 */
    uint32_t x = g_prng ? g_prng : 0x6D2B79F5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_prng = x;
    return x;
}

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
    if (!g_provisioned || !g_have_boot_id) { return -4; }
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (out_cap < wire) { return -1; }

    /* The 24-bit wire counter never wraps: refuse instead of reusing a nonce. */
    if (g_tx_ctr >= 0xFFFFFFu) { return -3; }
    uint32_t ctr = ++g_tx_ctr;

    /* Downlink packets are always from the master: both sides feed NODE_MASTER
     * to the nonce and the id stays off the wire. */
    uint32_t nid = has_node ? node_id : NODE_MASTER;

    /* Cleartext header = AEAD associated data. Little-endian; the 24-bit ctr's
     * implicit high byte is 0. */
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
                     out, (size_t)hdr,
                     (const uint8_t *)payload, (size_t)payload_len);
    return wire;
}

/* Verify and decrypt with a header of hdr bytes (node id in it when has_node)
 * and payload_len bytes of ciphertext before the tag. 0, or -2 on MAC failure. */
static int open_sealed(const uint8_t *in, int hdr, int has_node, int payload_len,
                       uint8_t *plain, sec_meta_t *meta)
{
    uint8_t type = SEC_VT_TYPE(in[0]);
    const uint8_t *p = in + 1;
    uint32_t boot_id; memcpy(&boot_id, p, 4); p += 4;
    uint32_t ctr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); p += 3;
    uint32_t nid = NODE_MASTER;
    if (has_node) { memcpy(&nid, p, 4); }

    uint8_t nonce[24];
    build_nonce(nonce, type, nid, boot_id, ctr);

    if (crypto_aead_unlock(plain,
                           in + hdr + payload_len, /* mac */
                           KEY, nonce,
                           in, (size_t)hdr,
                           in + hdr, (size_t)payload_len) != 0) {
        return -2;
    }
    meta->type = type;
    meta->node_id = nid;
    meta->boot_id = boot_id;
    meta->ctr = ctr;
    return 0;
}

int sec_unseal(const uint8_t *in, int in_len, sec_meta_t *meta,
               void *out_payload, int payload_len)
{
    if (!g_provisioned) { return -3; }
    if (in_len < 1) { return -1; }

    uint8_t vt = in[0];
    if (SEC_VT_VER(vt) != PROTO_VER) { return -4; }
    int has_node = SEC_TYPE_HAS_NODE(SEC_VT_TYPE(vt));
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (in_len < wire) { return -1; }

    uint8_t plain[WIRE_MAX];
    if (payload_len > (int)sizeof(plain)) { return -1; }
    int r = open_sealed(in, hdr, has_node, payload_len, plain, meta);
    if (r == 0) { memcpy(out_payload, plain, (size_t)payload_len); }
    crypto_wipe(plain, sizeof(plain));
    return r;
}

int sec_authentic_any_version(const uint8_t *in, int in_len)
{
    if (!g_provisioned || in_len < 1 || in_len > WIRE_MAX) { return 0; }
    /* v10 and v11 share the header, nonce and tag; they differ in which types
     * carry a node id, so try both header layouts. */
    uint8_t plain[WIRE_MAX];
    sec_meta_t meta;
    int ok = 0;
    for (int has_node = 0; has_node <= 1 && !ok; has_node++) {
        int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
        int payload_len = in_len - hdr - SEC_MAC_LEN;
        ok = payload_len >= 0 && open_sealed(in, hdr, has_node, payload_len, plain, &meta) == 0;
    }
    crypto_wipe(plain, sizeof(plain));
    return ok;
}

int sec_replay(sec_replay_t *st, uint32_t boot_id, uint32_t ctr)
{
    if (!st->have || boot_id != st->boot_id) {
        st->have = 1;
        st->boot_id = boot_id;
        st->max_ctr = ctr;
        return 1;
    }
    if (ctr > st->max_ctr) {
        st->max_ctr = ctr;
        return 1;
    }
    return 0;
}
