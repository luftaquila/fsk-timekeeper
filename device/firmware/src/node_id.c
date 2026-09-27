#include "node_id.h"

#include "config.h"
#include "protocol.h"
#include "nrf.h"

/* The one shared channel (sync word 0x12 for all — DESIGN §2.2). */
#define FREQ_HZ LORA_FREQ_MHZ

static uint32_t g_hi, g_lo;

void node_init(void)
{
    /* The chip's factory-unique id is this board's stable identity. Role is
     * decided at runtime from USB (master = a PC host enumerated us), and a
     * sensor transmits under its own chip id — the master auto-registers it on
     * first authenticated contact. Nothing about identity or role is hardcoded
     * here (DESIGN §2.3, §8). */
    g_hi = NRF_FICR->DEVICEID[1];
    g_lo = NRF_FICR->DEVICEID[0];
}

float node_freq_mhz(void) { return FREQ_HZ; }
uint32_t node_devid_hi(void) { return g_hi; }
uint32_t node_devid_lo(void) { return g_lo; }

uint32_t node_sender_id(void)
{
    /* The on-air sender identity = low 32 bits of the chip id, but never 0
     * (NODE_MASTER is the reserved id 0). The low word being exactly 0 is
     * astronomically unlikely for a factory id; fall back to the high word, then
     * to 1, so a sensor can never masquerade as the master. */
    if (g_lo != 0u) { return g_lo; }
    if (g_hi != 0u) { return g_hi; }
    return 1u;
}
