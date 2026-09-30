#include "keystore.h"

#include <string.h>

#include "crc32.h"
#include "errlog.h"
#include "nrf.h"

/* Top page of the application flash region (0x26000..0xF4000). The linker's
 * FLASH length stops at 0xF3000 (nrf52840_app.ld), so the app image never
 * covers this page and the key survives DFU. */
#define KEY_PAGE_ADDR  0x000F3000u
#define KEYSTORE_MAGIC 0x4B50534Bu /* 'KPSK' */
#define KEYSTORE_VER   1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  key[KEYSTORE_KEY_LEN];
    uint32_t crc; /* CRC-32 over magic + version + key */
} keystore_t;

#define KEYSTORE_CRC_LEN (4u + 4u + KEYSTORE_KEY_LEN)

int keystore_load(uint8_t key_out[KEYSTORE_KEY_LEN])
{
    const keystore_t *ks = (const keystore_t *)KEY_PAGE_ADDR;
    if (ks->magic != KEYSTORE_MAGIC || ks->version != KEYSTORE_VER) {
        return 0;
    }
    if (crc32_ieee(ks, KEYSTORE_CRC_LEN) != ks->crc) {
        return 0;
    }
    memcpy(key_out, ks->key, KEYSTORE_KEY_LEN);
    return 1;
}

/* The CPU stalls during NVMC operations; the bound only catches a controller
 * that never reports ready. */
static int nvmc_wait(void)
{
    for (uint32_t i = 0; i < 20000000u; i++) {
        if (NRF_NVMC->READY != NVMC_READY_READY_Busy) { return 1; }
    }
    el_note(EL_HW_TIMEOUT);
    return 0;
}

int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN])
{
    keystore_t ks;
    ks.magic = KEYSTORE_MAGIC;
    ks.version = KEYSTORE_VER;
    memcpy(ks.key, key, KEYSTORE_KEY_LEN);
    ks.crc = crc32_ieee(&ks, KEYSTORE_CRC_LEN);

    int ok = 1;
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();
    NRF_NVMC->ERASEPAGE = KEY_PAGE_ADDR;
    ok &= nvmc_wait();

    /* word-by-word (flash writes are 32-bit, word-aligned) */
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();
    const uint32_t *src = (const uint32_t *)(const void *)&ks;
    volatile uint32_t *dst = (volatile uint32_t *)KEY_PAGE_ADDR;
    unsigned words = (sizeof(keystore_t) + 3u) / 4u;
    for (unsigned i = 0; i < words && ok; i++) {
        dst[i] = src[i];
        ok &= nvmc_wait();
    }
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();

    /* verify the readback, then wipe the RAM copies */
    uint8_t check[KEYSTORE_KEY_LEN];
    ok = ok && keystore_load(check) && (memcmp(check, key, KEYSTORE_KEY_LEN) == 0);
    memset(&ks, 0, sizeof(ks));
    memset(check, 0, sizeof(check));
    return ok ? 0 : -1;
}
