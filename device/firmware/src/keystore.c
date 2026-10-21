#include "keystore.h"

#include <string.h>

#include "crc32.h"
#include "nvmc.h"

/* Top page of the app flash region (0x26000..0xF4000); the linker's FLASH stops below the radio
 * settings page at 0xF2000 (nrf52840_app.ld), so the app image never covers this page. */
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

int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN])
{
    keystore_t ks;
    ks.magic = KEYSTORE_MAGIC;
    ks.version = KEYSTORE_VER;
    memcpy(ks.key, key, KEYSTORE_KEY_LEN);
    ks.crc = crc32_ieee(&ks, KEYSTORE_CRC_LEN);
    int ok = nvmc_page_write(KEY_PAGE_ADDR, &ks, sizeof(ks)) == 0;

    /* verify the readback, then wipe the RAM copies */
    uint8_t check[KEYSTORE_KEY_LEN];
    ok = ok && keystore_load(check) && (memcmp(check, key, KEYSTORE_KEY_LEN) == 0);
    memset(&ks, 0, sizeof(ks));
    memset(check, 0, sizeof(check));
    return ok ? 0 : -1;
}
