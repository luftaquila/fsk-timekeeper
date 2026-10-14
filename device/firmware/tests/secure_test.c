#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Test the real production implementation, including counter exhaustion. */
#include "../src/secure.c"

static uint32_t now_ms;
static uint64_t entropy = UINT64_C(0x123456789abcdef0);
static int rng_ok = 1, key_ok = 1;
static unsigned rng_calls;
uint32_t board_millis(void) { return now_ms; }
uint32_t board_micros(void) { return now_ms * 1000u; }
int entropy_boot_id(uint64_t *out)
{
    rng_calls++;
    if (!rng_ok) { return 0; }
    *out = entropy;
    return 1;
}
int keystore_load(uint8_t out[KEYSTORE_KEY_LEN]) { memset(out, 0x42, KEYSTORE_KEY_LEN); return key_ok; }

static void packets(void)
{
    sec_init();
    assert(sec_ready() && sec_boot_id() == entropy);
    uplink_pl_t u = {.kind = UL_KIND_EDGES, .master_boot_id = UINT64_MAX, .tick = UINT64_MAX};
    u.u.edges.count = 1;
    uint8_t wire[WIRE_MAX], second[WIRE_MAX];
    int n = sec_seal(wire, sizeof wire, PKT_TYPE_UPLINK, 0x123443b0u, &u, sizeof u);
    assert(n == WIRE_UPLINK && n == 94);
    uplink_pl_t out;
    sec_meta_t meta;
    assert(sec_unseal(wire, n, &meta, &out, sizeof out) == 0);
    assert(meta.boot_id == entropy && meta.node_id == 0x123443b0u && meta.ctr == 1);
    assert(!memcmp(&u, &out, sizeof u));
    assert(sec_seal(second, sizeof second, PKT_TYPE_UPLINK, meta.node_id, &u, sizeof u) == n);
    assert(memcmp(wire, second, (size_t)n));
    for (int i = 0; i < n; i++) {
        wire[i] ^= 1;
        assert(sec_unseal(wire, n, &meta, &out, sizeof out) != 0);
        wire[i] ^= 1;
    }
    assert(sec_unseal(wire, n - 1, &meta, &out, sizeof out) != 0);
    assert(sec_unseal(wire, n + 1, &meta, &out, sizeof out) != 0);
    uint32_t ctr = g_tx_ctr;
    assert(sec_seal(second, 1, PKT_TYPE_UPLINK, 1, &u, sizeof u) == -1);
    assert(sec_seal(second, sizeof second, PKT_TYPE_UPLINK, 1, &u, -1) == -1);
    assert(sec_seal(second, sizeof second, PKT_TYPE_UPLINK, 1, &u, INT_MAX) == -1);
    assert(g_tx_ctr == ctr);
    assert(sec_unseal(wire, n, &meta, &out, -1) == -1);
    assert(sec_unseal(wire, n, &meta, &out, INT_MAX) == -1);
    beacon_pl_t b = {0}, bout;
    n = sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b);
    assert(n == 109 && sec_unseal(wire, n, &meta, &bout, sizeof bout) == 0);
    assert(sec_authentic_any_version(wire, n));
    g_tx_ctr = 0xfffffeu;
    assert(sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b) == n);
    assert(!sec_ready());
    assert(sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b) == -3);
    sec_reload(); sec_service(now_ms + 10000u);
    assert(g_tx_ctr == 0xffffffu && !sec_ready());
    puts("PASS secure_packets_counter");
}

static void rng_recovery(void)
{
    rng_ok = 0;
    sec_init();
    assert(sec_provisioned() && !sec_ready() && !sec_boot_ready());
    beacon_pl_t b = {0}; uint8_t wire[WIRE_MAX];
    assert(sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b) == -4);
    rng_ok = 1; entropy++;
    unsigned calls = rng_calls;
    sec_service(999); assert(rng_calls == calls);
    now_ms = 1000; sec_service(now_ms);
    assert(sec_ready() && sec_boot_id() == entropy && g_tx_ctr == 0);
    assert(sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b) > 0);
    sec_service(2000); sec_reload();
    assert(g_tx_ctr == 1 && rng_calls == calls + 1);
    key_ok = 0; sec_reload(); assert(!sec_ready() && !sec_provisioned());
    key_ok = 1; sec_reload(); assert(sec_ready() && g_tx_ctr == 1);
    puts("PASS secure_rng_recovery");
}

static void replay(void)
{
    sec_replay_t r = {0};
    assert(!sec_replay(&r, 0, 0));
    assert(!r.have);
    assert(sec_replay(&r, 0, 100)); /* boot zero is valid */
    assert(!sec_replay(&r, 0, 100));
    assert(sec_replay(&r, UINT64_MAX, 1));
    assert(!sec_replay(&r, 0, 101)); /* A -> B -> A */
    assert(r.boot_id == UINT64_MAX && r.max_ctr == 1);
    assert(sec_replay(&r, UINT64_MAX, 2));
    for (uint64_t i = 1; i <= SEC_RETIRED_BOOTS; i++) { assert(sec_replay(&r, i, 1)); }
    assert(!sec_replay(&r, UINT64_MAX, 3));
    assert(sec_replay(&r, 0, 101)); /* documented bounded-history limit */
    puts("PASS secure_recent_sessions");
}

static void legacy_authentication(void)
{
    /* Previously deployed v12 header and nonce, authenticated only for diagnostics. */
    uint8_t wire[60] = {SEC_VT(12, PKT_TYPE_BEACON)}, nonce[24] = {0x57, PKT_TYPE_BEACON};
    uint32_t boot = 0x12345678u, ctr = 1;
    memcpy(wire + 1, &boot, 4); wire[5] = 1;
    memcpy(nonce + 6, &boot, 4); memcpy(nonce + 10, &ctr, 4);
    uint8_t payload[36] = {0};
    crypto_aead_lock(wire + 8, wire + 44, KEY, nonce, wire, 8, payload, 36);
    assert(sec_authentic_any_version(wire, sizeof wire));
    beacon_pl_t out; sec_meta_t meta;
    assert(sec_unseal(wire, sizeof wire, &meta, &out, sizeof out) != 0);
    wire[59] ^= 1; assert(!sec_authentic_any_version(wire, sizeof wire));
    puts("PASS secure_legacy_diagnostic");
}

int main(void) { packets(); rng_recovery(); replay(); legacy_authentication(); return 0; }
