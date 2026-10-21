#include "radio_cfg.h"

#include "crc32.h"
#include "nvmc.h"

/* The page below the key page (keystore.c); the linker's FLASH length stops here. */
#define RADIO_CFG_PAGE_ADDR 0x000F2000u
#define RADIO_CFG_MAGIC     0x47464352u /* 'RCFG' */
#define RADIO_CFG_VER       1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t freq_khz;
    int32_t  power_dbm;
    uint32_t lbt;
    uint32_t crc; /* CRC-32 over everything before it */
} radio_cfg_rec_t;

#define RADIO_CFG_CRC_LEN (sizeof(radio_cfg_rec_t) - 4u)

static int load_rec(radio_cfg_t *out)
{
    const radio_cfg_rec_t *r = (const radio_cfg_rec_t *)RADIO_CFG_PAGE_ADDR;
    if (r->magic != RADIO_CFG_MAGIC || r->version != RADIO_CFG_VER) { return 0; }
    if (crc32_ieee(r, RADIO_CFG_CRC_LEN) != r->crc) { return 0; }
    if (r->power_dbm < RADIO_POWER_MIN_DBM || r->power_dbm > RADIO_POWER_MAX_DBM) { return 0; }
    radio_cfg_t c = { r->freq_khz, (int8_t)r->power_dbm, (uint8_t)r->lbt };
    if (r->lbt > 1u || !radio_cfg_valid(&c)) { return 0; }
    *out = c;
    return 1;
}

void radio_cfg_load(radio_cfg_t *out)
{
    if (!load_rec(out)) {
        radio_cfg_t def = RADIO_CFG_DEFAULT;
        *out = def;
    }
}

int radio_cfg_write(const radio_cfg_t *c)
{
    if (!radio_cfg_valid(c)) { return -1; }
    radio_cfg_rec_t r = { RADIO_CFG_MAGIC, RADIO_CFG_VER, c->freq_khz, c->power_dbm, c->lbt, 0u };
    r.crc = crc32_ieee(&r, RADIO_CFG_CRC_LEN);
    int ok = nvmc_page_write(RADIO_CFG_PAGE_ADDR, &r, sizeof(r)) == 0;

    radio_cfg_t check;
    ok = ok && load_rec(&check) && check.freq_khz == c->freq_khz && check.power_dbm == c->power_dbm &&
         check.lbt == c->lbt;
    return ok ? 0 : -1;
}
