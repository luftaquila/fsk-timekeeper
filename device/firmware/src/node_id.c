#include "node_id.h"

#include "nrf.h"

static uint32_t g_hi, g_lo;

void node_init(void)
{
    g_hi = NRF_FICR->DEVICEID[1];
    g_lo = NRF_FICR->DEVICEID[0];
}

uint32_t node_devid_hi(void) { return g_hi; }
uint32_t node_devid_lo(void) { return g_lo; }

uint32_t node_sender_id(void)
{
    /* A zero low word (astronomically unlikely) falls back to the high word,
     * then to 1, so a sensor can never claim the master's id 0. */
    if (g_lo != 0u) { return g_lo; }
    if (g_hi != 0u) { return g_hi; }
    return 1u;
}
