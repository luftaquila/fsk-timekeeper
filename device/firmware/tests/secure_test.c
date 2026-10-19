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
int entropy_poll(uint8_t *out) { static uint8_t b; if (!rng_ok) { return -1; } *out = ++b; return 1; }
int keystore_load(uint8_t out[KEYSTORE_KEY_LEN]) { memset(out, 0x42, KEYSTORE_KEY_LEN); return key_ok; }

static void packets(void)
{
    sec_init();
    assert(sec_ready() && sec_boot_id() == entropy);
    uplink_pl_t u = {.kind = UL_KIND_EDGES, .master_boot_id = UINT64_MAX, .tick = PROTO_TICK_MAX};
    u.u.edges.count = 1;
    uint8_t wire[WIRE_MAX], second[WIRE_MAX];
    int n = sec_seal_key(KEY, wire, sizeof wire, PKT_TYPE_UPLINK, 0x123443b0u, &u, sizeof u);
    assert(n == WIRE_UPLINK && n == 91);
    uplink_pl_t out;
    sec_meta_t meta;
    assert(sec_unseal_key(KEY, wire, n, &meta, &out, sizeof out) == 0);
    assert(meta.boot_id == entropy && meta.node_id == 0x123443b0u && meta.ctr == 1);
    assert(!memcmp(&u, &out, sizeof u));
    assert(sec_seal_key(KEY, second, sizeof second, PKT_TYPE_UPLINK, meta.node_id, &u, sizeof u) == n);
    assert(memcmp(wire, second, (size_t)n));
    for (int i = 0; i < n; i++) {
        sec_meta_t saved_meta = meta;
        uplink_pl_t saved_out = out;
        wire[i] ^= 1;
        assert(sec_unseal_key(KEY, wire, n, &meta, &out, sizeof out) != 0);
        assert(saved_meta.type == meta.type && saved_meta.node_id == meta.node_id &&
               saved_meta.boot_id == meta.boot_id && saved_meta.ctr == meta.ctr);
        assert(!memcmp(&saved_out, &out, sizeof(out)));
        wire[i] ^= 1;
    }
    assert(sec_unseal_key(KEY, wire, n - 1, &meta, &out, sizeof out) != 0);
    assert(sec_unseal_key(KEY, wire, n + 1, &meta, &out, sizeof out) != 0);
    uint32_t ctr = g_tx_ctr;
    assert(sec_seal_key(KEY, second, 1, PKT_TYPE_UPLINK, 1, &u, sizeof u) == -1);
    assert(sec_seal_key(KEY, second, sizeof second, PKT_TYPE_UPLINK, 1, &u, -1) == -1);
    assert(sec_seal_key(KEY, second, sizeof second, PKT_TYPE_UPLINK, 1, &u, INT_MAX) == -1);
    assert(g_tx_ctr == ctr);
    assert(sec_unseal_key(KEY, wire, n, &meta, &out, -1) == -1);
    assert(sec_unseal_key(KEY, wire, n, &meta, &out, INT_MAX) == -1);
    beacon_pl_t b = {0}, bout;
    n = sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b);
    assert(n == 87 && sec_unseal(wire, n, &meta, &bout, sizeof bout) == 0);
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
    /* The deployed v15 control keys include their version. They remain readable
     * for mismatch diagnostics, never as live v16 packets. */
    const uint8_t types[] = {PKT_TYPE_BEACON, PKT_TYPE_JOIN, PKT_TYPE_OFFER};
    const int lengths[] = {81, 24, 55};
    for (unsigned i = 0; i < 3; i++) {
        uint8_t old[WIRE_MAX] = {SEC_VT(15, types[i])}, plain[81] = {0}, key[32];
        uint64_t boot64 = UINT64_C(0x123456789abcdef0);
        uint32_t node = types[i] == PKT_TYPE_JOIN ? 0xabcd1234u : 0;
        int hdr = node ? 16 : 12, size = hdr + lengths[i] + 16;
        memcpy(old + 1, &boot64, 8); old[9] = 1;
        if (node) { memcpy(old + 12, &node, 4); }
        uint8_t context[16] = {'F','S','K',15,'C',types[i],0,0};
        if (types[i] == PKT_TYPE_BEACON) { memcpy(context + 8, &boot64, 8); }
        crypto_blake2b_keyed(key, 32, KEY, sizeof(KEY), context, sizeof(context));
        build_nonce(nonce, types[i], node, boot64, 1);
        crypto_aead_lock(old + hdr, old + hdr + lengths[i], key, nonce, old, (size_t)hdr, plain, (size_t)lengths[i]);
        assert(sec_authentic_any_version(old, size));
        assert(sec_unseal(old, size, &meta, &out, sizeof(out)) == -4);
        old[size - 1] ^= 1; assert(!sec_authentic_any_version(old, size));
    }
    puts("PASS secure_legacy_diagnostic");
}

static void connection_keys(void)
{
    sec_init();
    uint8_t ns[16] = {1}, nm[16] = {2}; sec_link_keys_t k, changed;
    assert(sec_link_keys(&k, 123, 456, 789, ns, nm, 1));
    static const uint8_t expected[32] = {0x6a, 0x2f, 0x29, 0xe0, 0xfd, 0xa5, 0x2d, 0x2e, 0x52, 0xbd, 0x79, 0x30, 0x69, 0x79, 0x99, 0xe2, 0x03, 0xe0, 0xf3, 0xdb, 0x17, 0x66, 0xa8, 0x29, 0x4e, 0x08, 0x57, 0xfa, 0x78, 0x72, 0xc3, 0x00};
    assert(!memcmp(k.data, expected, 32)); /* independent Python hashlib vector */
    assert(memcmp(k.data, k.confirm, 32));
    for (unsigned field = 0; field < 6; field++) {
        uint8_t a[16], b[16]; memcpy(a, ns, 16); memcpy(b, nm, 16);
        if (field == 3) { a[15]++; }
        if (field == 4) { b[15]++; }
        assert(sec_link_keys(&changed, 123 + (field == 0), 456 + (field == 1), 789 + (field == 2), a, b, 1 + (field == 5)));
        assert(memcmp(&changed, &k, sizeof(k)));
    }
    /* The upper boot/node bits must still distinguish keys; only link width changed. */
    for (unsigned field = 0; field < 3; field++) {
        assert(sec_link_keys(&changed, 123 | (field == 0 ? UINT64_C(1) << 63 : 0),
                            456 | (field == 1 ? UINT32_C(1) << 31 : 0),
                            789 | (field == 2 ? UINT64_C(1) << 63 : 0), ns, nm, 1));
        assert(memcmp(&changed, &k, sizeof(k)));
    }
    uint8_t wire[WIRE_MAX]; uplink_pl_t p = {0}, out; sec_meta_t m;
    int n = sec_seal_key(k.data, wire, sizeof(wire), PKT_TYPE_UPLINK, 456, &p, sizeof(p));
    assert(n == WIRE_UPLINK);
    assert(sec_unseal_key(k.data, wire, n, &m, &out, sizeof(out)) == 0);
    assert(sec_unseal_key(k.confirm, wire, n, &m, &out, sizeof(out)) != 0);
    assert(sec_unseal_key(changed.data, wire, n, &m, &out, sizeof(out)) != 0);
    assert(sec_unseal(wire, n, &m, &out, sizeof(out)) != 0);
    assert(sec_seal(wire, sizeof(wire), PKT_TYPE_UPLINK, 456, &p, sizeof(p)) == -5);
    assert(sec_seal_key(k.data, wire, sizeof(wire), 15, 456, &p, sizeof(p)) < 0);
    uint32_t high = 0;
    assert(sec_accept_counter(&high, 2) && high == 2);
    assert(!sec_accept_counter(&high, 1) && !sec_accept_counter(&high, 2) && !sec_accept_counter(&high, 0));
    assert(sec_accept_counter(&high, 0xffffff) && !sec_accept_counter(&high, 0x1000000));
    puts("PASS secure_connection_key_isolation");
}

static void compact_payload_validation(void)
{
    sec_init();
    admission_beacon_t a = {.base = {.flags = BEACON_ADMISSION, .m_tx_prev = PROTO_TICK_MAX},
                             .schedule = {SLOT_ROLE_REQUEST, SLOT_ROLE_OFFER | 4, SLOT_ROLE_CONFIRM | 4}}, out;
    uint8_t wire[WIRE_MAX], plain[PL_ADMISSION], key[32], nonce[24];
    sec_meta_t meta;
    int n = sec_seal(wire, sizeof(wire), PKT_TYPE_BEACON, 0, &a, sizeof(a));
    assert(n == WIRE_ADMISSION && sec_unseal(wire, n, &meta, &out, sizeof(out)) == 0);
    assert(!memcmp(&a, &out, sizeof(a)));
    assert(sec_authentic_any_version(wire, n));
    a.schedule[0] = SLOT_ROLE_DATA | 7;
    uint32_t counter = g_tx_ctr;
    assert(sec_seal(wire, sizeof(wire), PKT_TYPE_BEACON, 0, &a, sizeof(a)) == -1 && g_tx_ctr == counter);
    a.schedule[0] = SLOT_ROLE_REQUEST;
    /* A valid authentication tag must not bypass validation of packed fields.
     * Give the malformed packet a fresh counter, as a faulty fleet peer would. */
    assert(protocol_encode(plain, sizeof(plain), PKT_TYPE_BEACON, &a, sizeof(a)) == PL_ADMISSION);
    plain[PL_BEACON] |= 31u; /* reserved slot code */
    wire[9] = 2;
    control_key(key, PKT_TYPE_BEACON, g_boot_id);
    build_nonce(nonce, PKT_TYPE_BEACON, 0, g_boot_id, 2);
    crypto_aead_lock(wire + SEC_HDR_DL, wire + SEC_HDR_DL + PL_ADMISSION, key, nonce,
                     wire, SEC_HDR_DL, plain, sizeof(plain));
    admission_beacon_t saved = out;
    assert(sec_unseal(wire, n, &meta, &out, sizeof(out)) == -1);
    assert(!memcmp(&saved, &out, sizeof(out)) && meta.ctr == 1);

    offer_pl_t offer = {.node_id = 0xfedc0001u, .sensor_boot = UINT64_MAX, .link_id = UINT32_MAX}, offered;
    memset(offer.ns, 0x55, sizeof(offer.ns)); memset(offer.nm, 0xaa, sizeof(offer.nm));
    n = sec_seal(wire, sizeof(wire), PKT_TYPE_OFFER, 0, &offer, sizeof(offer));
    assert(n == WIRE_OFFER && sec_unseal(wire, n, &meta, &offered, sizeof(offered)) == 0);
    assert(!memcmp(&offer, &offered, sizeof(offer)));
    puts("PASS secure_compact_payload_validation");
}

static void challenge_pool(void)
{
    sec_init(); uint8_t nonce[16], last[16] = {0};
    assert(!sec_challenge(nonce));
    for (unsigned i = 0; i < 37; i++) sec_service(now_ms);
    assert(g_challenge_count == 2 && g_challenge_fill == 5);
    assert(sec_challenge(last) && sec_challenge(nonce) && memcmp(nonce, last, 16));
    assert(!sec_challenge(nonce) && g_challenge_fill == 5);
    for (unsigned i = 0; i < 11; i++) sec_service(now_ms);
    assert(sec_challenge(nonce) && memcmp(nonce, last, 16));
    rng_ok = 0; sec_service(now_ms); assert(!sec_challenge(nonce) && sec_ready()); rng_ok = 1;
    for (unsigned i = 0; i < 100; i++) sec_service(now_ms);
    assert(g_challenge_count == 5);
    for (unsigned i = 0; i < 5; i++) { memcpy(last, nonce, 16); assert(sec_challenge(nonce) && memcmp(nonce, last, 16)); }
    assert(!sec_challenge(nonce));
    puts("PASS secure_nonblocking_challenge_pool");
}

int main(void) { packets(); rng_recovery(); legacy_authentication(); connection_keys(); compact_payload_validation(); challenge_pool(); return 0; }
