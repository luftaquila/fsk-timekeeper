#include "keystore.h"

#include <string.h>

#include "nrf.h"

/* Top page of the application flash region (0x26000..0xF4000). The linker's
 * FLASH length stops at 0xF3000 (nrf52840_app.ld), so the app never occupies
 * this page and the app .uf2 never rewrites it — the key persists across DFU. */
#define KEY_PAGE_ADDR  0x000F3000u
#define KEYSTORE_MAGIC 0x4B50534Bu /* 'KPSK' */
#define KEYSTORE_VER   1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  key[KEYSTORE_KEY_LEN];
    uint32_t crc; /* CRC32 over magic+version+key (the preceding 40 bytes) */
} keystore_t;

#define KEYSTORE_CRC_LEN (4u + 4u + KEYSTORE_KEY_LEN) /* bytes covered by crc */

/* Table-less CRC32 (poly 0xEDB88320), only used at boot / provisioning. */
static uint32_t crc32(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

int keystore_load(uint8_t key_out[KEYSTORE_KEY_LEN])
{
    const keystore_t *ks = (const keystore_t *)KEY_PAGE_ADDR;
    if (ks->magic != KEYSTORE_MAGIC || ks->version != KEYSTORE_VER) {
        return 0; /* unprovisioned / wrong format */
    }
    if (crc32((const uint8_t *)ks, KEYSTORE_CRC_LEN) != ks->crc) {
        return 0; /* corrupt / interrupted write */
    }
    memcpy(key_out, ks->key, KEYSTORE_KEY_LEN);
    return 1;
}

static void nvmc_wait(void)
{
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) { /* CPU stalls during op */ }
}

int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN])
{
    keystore_t ks;
    ks.magic = KEYSTORE_MAGIC;
    ks.version = KEYSTORE_VER;
    memcpy(ks.key, key, KEYSTORE_KEY_LEN);
    ks.crc = crc32((const uint8_t *)&ks, KEYSTORE_CRC_LEN);

    /* erase the page */
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos);
    nvmc_wait();
    NRF_NVMC->ERASEPAGE = KEY_PAGE_ADDR;
    nvmc_wait();

    /* write word-by-word (flash writes are 32-bit, word-aligned) */
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos);
    nvmc_wait();
    const uint32_t *src = (const uint32_t *)(const void *)&ks;
    volatile uint32_t *dst = (volatile uint32_t *)KEY_PAGE_ADDR;
    unsigned words = (sizeof(keystore_t) + 3u) / 4u;
    for (unsigned i = 0; i < words; i++) {
        dst[i] = src[i];
        nvmc_wait();
    }
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos);
    nvmc_wait();

    /* verify the readback, then wipe the RAM copies */
    uint8_t check[KEYSTORE_KEY_LEN];
    int ok = keystore_load(check) && (memcmp(check, key, KEYSTORE_KEY_LEN) == 0);
    memset(&ks, 0, sizeof(ks));
    memset(check, 0, sizeof(check));
    return ok ? 0 : -1;
}
