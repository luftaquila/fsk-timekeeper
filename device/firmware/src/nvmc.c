#include "nvmc.h"

#include "errlog.h"
#include "nrf.h"

/* The CPU stalls during NVMC operations; the bound only catches a never-ready controller. */
static int nvmc_wait(void)
{
    for (uint32_t i = 0; i < 20000000u; i++) {
        if (NRF_NVMC->READY != NVMC_READY_READY_Busy) { return 1; }
    }
    el_note(EL_HW_TIMEOUT);
    return 0;
}

int nvmc_page_write(uint32_t page_addr, const void *data, unsigned len)
{
    int ok = 1;
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();
    NRF_NVMC->ERASEPAGE = page_addr;
    ok &= nvmc_wait();

    /* word-by-word (flash writes are 32-bit, word-aligned) */
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();
    const uint32_t *src = (const uint32_t *)data;
    volatile uint32_t *dst = (volatile uint32_t *)page_addr;
    unsigned words = (len + 3u) / 4u;
    for (unsigned i = 0; i < words && ok; i++) {
        dst[i] = src[i];
        ok &= nvmc_wait();
    }
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos);
    ok &= nvmc_wait();
    return ok ? 0 : -1;
}
