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
    uplink_pl_t u = {.kind = UL_KIND_EDGES, .master_boot_id = UINT64_MAX, .tick = UINT64_C(0xffffffffffff)};
    u.u.edges.count = 1;
    uint8_t wire[WIRE_MAX], second[WIRE_MAX];
    int n = sec_seal_key(KEY, wire, sizeof wire, PKT_TYPE_UPLINK, 0x123443b0u, &u, sizeof u);
    assert(n == WIRE_UPLINK);
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
    assert(n == WIRE_BEACON && sec_unseal(wire, n, &meta, &bout, sizeof bout) == 0);
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

static void other_version_diagnostic(void)
{
    /* Another protocol version authenticates (counted in ver_drop) but is never accepted. */
    sec_init();
    beacon_pl_t b = {0}, out; sec_meta_t meta;
    uint8_t wire[WIRE_MAX], plain[PL_BEACON], key[32], nonce[24];
    int n = sec_seal(wire, sizeof wire, PKT_TYPE_BEACON, 0, &b, sizeof b);
    assert(n == WIRE_BEACON && protocol_encode(plain, sizeof plain, PKT_TYPE_BEACON, &b, sizeof b) == PL_BEACON);
    wire[0] = SEC_VT(PROTO_VER + 1u, PKT_TYPE_BEACON);
    control_key_version(key, PKT_TYPE_BEACON, g_boot_id, PROTO_VER + 1u);
    build_nonce(nonce, PKT_TYPE_BEACON, 0, g_boot_id, wire[9]);
    crypto_aead_lock(wire + SEC_HDR_DL, wire + SEC_HDR_DL + PL_BEACON, key, nonce, wire, SEC_HDR_DL, plain, sizeof plain);
    assert(sec_authentic_any_version(wire, n));
    assert(sec_unseal(wire, n, &meta, &out, sizeof out) == -4);
    wire[n - 1] ^= 1; assert(!sec_authentic_any_version(wire, n));
    puts("PASS secure_other_version_diagnostic");
}

static void connection_keys(void)
{
    sec_init();
    uint8_t ns[16] = {1}, nm[16] = {2}; sec_link_keys_t k, changed;
    assert(sec_link_keys(&k, 123, 456, 789, ns, nm, 1));
    static const uint8_t expected[32] = {0x94, 0x45, 0x2b, 0x82, 0x00, 0xc0, 0xa5, 0xe4, 0x37, 0x88, 0x27, 0x63, 0x26, 0xbd, 0x04, 0x79, 0xf7, 0x1b, 0xd0, 0x07, 0xb0, 0x6b, 0x58, 0x7b, 0xfc, 0xf2, 0xa7, 0xce, 0x9c, 0x4b, 0xec, 0x88};
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
    admission_beacon_t a = {.base = {.flags = BEACON_ADMISSION, .m_tx_prev = UINT64_C(0xffffffffffff)},
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
    /* A valid tag must not skip packed-field checks (fresh counter, like a faulty fleet peer). */
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

static void authenticated_padding(void)
{
    sec_init();
    uplink_pl_t u = {.kind = UL_KIND_SYNC, .beacon_ctr = 0xffffff}, out;
    uint8_t wire[WIRE_MAX], plain[WIRE_UPLINK_ADMISSION - SEC_HDR_UL - SEC_MAC_LEN], nonce[24];
    sec_meta_t meta;
    int n = sec_seal_uplink(KEY, wire, sizeof(wire), 123, &u, sizeof(u), WIRE_ADMISSION);
    assert(n == WIRE_ADMISSION && sec_unseal_key(KEY, wire, n, &meta, &out, sizeof(out)) == 0);
    assert(!memcmp(&u, &out, sizeof(u)));
    uplink_pl_t saved = out;
    for (int i = 0; i < n; i++) {
        wire[i] ^= 1;
        assert(sec_unseal_key(KEY, wire, n, &meta, &out, sizeof(out)) < 0);
        assert(!memcmp(&out, &saved, sizeof(out)) && meta.ctr == 1);
        wire[i] ^= 1;
    }
    assert(protocol_encode(plain, sizeof(plain), PKT_TYPE_UPLINK, &u, sizeof(u)) == PL_UPLINK);
    memset(plain + PL_UPLINK, 0, sizeof(plain) - PL_UPLINK);
    plain[PL_UPLINK] = 1; /* valid tag, noncanonical padding must still fail */
    build_nonce(nonce, PKT_TYPE_UPLINK, 123, g_boot_id, 1);
    crypto_aead_lock(wire + SEC_HDR_UL, wire + n - SEC_MAC_LEN, KEY, nonce,
                     wire, SEC_HDR_UL, plain, sizeof(plain));
    assert(sec_unseal_key(KEY, wire, n, &meta, &out, sizeof(out)) == -1);
    assert(!memcmp(&out, &saved, sizeof(out)) && meta.ctr == 1);
    uint32_t ctr = g_tx_ctr;
    assert(sec_seal_uplink(KEY, wire, sizeof(wire), 123, &u, sizeof(u), WIRE_ADMISSION - 1) == -1);
    assert(sec_seal_uplink(KEY, wire, WIRE_ADMISSION - 1, 123, &u, sizeof(u), WIRE_ADMISSION) == -1);
    assert(g_tx_ctr == ctr);
    puts("PASS secure_authenticated_equal_length_padding");
}

static void challenge_pool(void)
{
    sec_init(); uint8_t nonce[16], last[16] = {0};
    assert(!sec_challenge(nonce));
    sec_service(now_ms);
    assert(!sec_challenge(nonce)); /* one service call never fills a whole challenge */
    for (unsigned i = 0; i < 100; i++) sec_service(now_ms);
    unsigned drawn = 0;
    while (sec_challenge(nonce)) {
        assert(memcmp(nonce, last, 16) && ++drawn < 100);
        memcpy(last, nonce, 16);
    }
    assert(drawn > 1);
    rng_ok = 0;
    for (unsigned i = 0; i < 100; i++) sec_service(now_ms);
    assert(!sec_challenge(nonce) && sec_ready());
    rng_ok = 1;
    for (unsigned i = 0; i < 100; i++) sec_service(now_ms);
    assert(sec_challenge(nonce) && memcmp(nonce, last, 16));
    puts("PASS secure_nonblocking_challenge_pool");
}

int main(void) { packets(); rng_recovery(); other_version_diagnostic(); connection_keys(); compact_payload_validation(); authenticated_padding(); challenge_pool(); return 0; }
