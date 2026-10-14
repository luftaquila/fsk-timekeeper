#include "secure.h"

#include <string.h>

#include "board.h"
#include "errlog.h"
#include "protocol.h"
#include "keystore.h"
#include "monocypher.h"
#include "entropy.h"

/* Nonce domain byte ('W' for FSK-WL) separates this use of the key. */
#define NONCE_DOMAIN 0x57u

static uint8_t  KEY[KEYSTORE_KEY_LEN];
static int      g_provisioned;
static int      g_have_boot_id;
static uint64_t g_boot_id;
static uint32_t g_tx_ctr;
static uint32_t g_prng;

static uint32_t g_rng_retry_ms;

void sec_init(void)
{
    g_have_boot_id = entropy_boot_id(&g_boot_id);
    g_tx_ctr = 0;
    g_rng_retry_ms = board_millis() + 1000u;
    g_prng = (uint32_t)g_boot_id ^ (uint32_t)(g_boot_id >> 32) ^ board_micros() ^ 0x9E3779B9u;
    sec_reload();
}

void sec_service(uint32_t now_ms)
{
    if (g_have_boot_id || (int32_t)(now_ms - g_rng_retry_ms) < 0) { return; }
    g_have_boot_id = entropy_boot_id(&g_boot_id);
    g_rng_retry_ms = board_millis() + 1000u;
    /* No packet has been sealed in this session yet. Never reset a live counter. */
    if (g_have_boot_id) { g_prng = (uint32_t)g_boot_id ^ (uint32_t)(g_boot_id >> 32); }
}

int sec_boot_ready(void) { return g_have_boot_id; }
int sec_ready(void) { return g_provisioned && g_have_boot_id && g_tx_ctr < 0xffffffu; }

void sec_reload(void)
{
    g_provisioned = keystore_load(KEY);
}

int sec_provisioned(void) { return g_provisioned; }

uint64_t sec_boot_id(void) { return g_boot_id; }

uint32_t sec_random(void)
{
    /* xorshift32 */
    uint32_t x = g_prng ? g_prng : 0x6D2B79F5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_prng = x;
    return x;
}

static void build_nonce(uint8_t nonce[24], uint8_t type, uint32_t node_id,
                        uint64_t boot_id, uint32_t ctr)
{
    memset(nonce, 0, 24);
    nonce[0] = NONCE_DOMAIN;
    nonce[1] = type;
    memcpy(&nonce[2], &node_id, 4);
    memcpy(&nonce[6], &boot_id, 8);
    memcpy(&nonce[14], &ctr, 4);
}

int sec_seal(uint8_t *out, int out_cap, uint8_t type, uint32_t node_id,
             const void *payload, int payload_len)
{
    if (!g_provisioned || !g_have_boot_id) { return -4; }
    int has_node = SEC_TYPE_HAS_NODE(type);
    int hdr = has_node ? SEC_HDR_UL : SEC_HDR_DL;
    if (payload_len < 0 || payload_len > WIRE_MAX - hdr - SEC_MAC_LEN) { return -1; }
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
    memcpy(p, &g_boot_id, 8); p += 8;
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
    uint64_t boot_id; memcpy(&boot_id, p, 8); p += 8;
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
    if (payload_len < 0 || payload_len > WIRE_MAX - hdr - SEC_MAC_LEN) { return -1; }
    int wire = hdr + payload_len + SEC_MAC_LEN;
    if (in_len != wire) { return -1; }

    uint8_t plain[WIRE_MAX];
    int r = open_sealed(in, hdr, has_node, payload_len, plain, meta);
    if (r == 0) { memcpy(out_payload, plain, (size_t)payload_len); }
    crypto_wipe(plain, sizeof(plain));
    return r;
}

int sec_authentic_any_version(const uint8_t *in, int in_len)
{
    if (!g_provisioned || in_len < 1 || in_len > WIRE_MAX) { return 0; }
    unsigned ver = SEC_VT_VER(in[0]);
    if (ver >= 10u && ver <= 12u) {
        for (int has_node = 0; has_node <= 1; has_node++) {
            int hdr = has_node ? 12 : 8;
            int len = in_len - hdr - SEC_MAC_LEN;
            if (len < 0) { continue; }
            uint32_t boot, node = NODE_MASTER;
            memcpy(&boot, in + 1, 4);
            uint32_t ctr = (uint32_t)in[5] | ((uint32_t)in[6] << 8) | ((uint32_t)in[7] << 16);
            if (has_node) { memcpy(&node, in + 8, 4); }
            uint8_t nonce[24] = {NONCE_DOMAIN, SEC_VT_TYPE(in[0])};
            memcpy(nonce + 2, &node, 4);
            memcpy(nonce + 6, &boot, 4);
            memcpy(nonce + 10, &ctr, 4);
            uint8_t plain[WIRE_MAX];
            int ok = crypto_aead_unlock(plain, in + hdr + len, KEY, nonce, in, (size_t)hdr,
                                        in + hdr, (size_t)len) == 0;
            crypto_wipe(plain, sizeof(plain));
            if (ok) { return 1; }
        }
        return 0;
    }
    int hdr = SEC_TYPE_HAS_NODE(SEC_VT_TYPE(in[0])) ? SEC_HDR_UL : SEC_HDR_DL;
    int len = in_len - hdr - SEC_MAC_LEN;
    if (len < 0) { return 0; }
    uint8_t plain[WIRE_MAX];
    sec_meta_t meta;
    int ok = open_sealed(in, hdr, SEC_TYPE_HAS_NODE(SEC_VT_TYPE(in[0])), len, plain, &meta) == 0;
    crypto_wipe(plain, sizeof(plain));
    return ok;
}
