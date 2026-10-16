#include "entropy.h"

#include "board.h"
#include "errlog.h"
#include "nrf.h"

static int running;
static uint32_t started;

int entropy_poll(uint8_t *out)
{
    if (!running) {
        NRF_RNG->CONFIG = (RNG_CONFIG_DERCEN_Enabled << RNG_CONFIG_DERCEN_Pos);
        NRF_RNG->EVENTS_VALRDY = 0;
        NRF_RNG->TASKS_START = 1;
        started = board_micros();
        running = 1;
    }
    if (NRF_RNG->EVENTS_VALRDY) {
        *out = (uint8_t)NRF_RNG->VALUE;
        NRF_RNG->EVENTS_VALRDY = 0;
        NRF_RNG->TASKS_STOP = 1;
        running = 0;
        return 1;
    }
    if ((uint32_t)(board_micros() - started) >= 5000u) {
        NRF_RNG->TASKS_STOP = 1;
        running = 0;
        el_note(EL_HW_TIMEOUT);
        return -1;
    }
    return 0;
}

int entropy_boot_id(uint64_t *out)
{
    running = 0;
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
