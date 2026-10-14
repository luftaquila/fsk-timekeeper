#include "entropy.h"

#include "board.h"
#include "errlog.h"
#include "nrf.h"

int entropy_boot_id(uint64_t *out)
{
    NRF_RNG->CONFIG = (RNG_CONFIG_DERCEN_Enabled << RNG_CONFIG_DERCEN_Pos);
    NRF_RNG->TASKS_START = 1;
    uint64_t value = 0;
    int ok = 1;
    for (unsigned i = 0; i < 8u && ok; i++) {
        NRF_RNG->EVENTS_VALRDY = 0;
        uint32_t start = board_micros();
        while (!NRF_RNG->EVENTS_VALRDY) {
            if ((uint32_t)(board_micros() - start) >= 5000u) { ok = 0; break; }
        }
        value = (value << 8) | (NRF_RNG->VALUE & 0xffu);
    }
    NRF_RNG->TASKS_STOP = 1;
    if (!ok) { el_note(EL_HW_TIMEOUT); return 0; }
    *out = value;
    return 1;
}
