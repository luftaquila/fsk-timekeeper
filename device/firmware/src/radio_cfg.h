/* Per-board radio settings, set over USB (`R`, proto_usb.h) in their own flash page below the key
 * page (nrf52840_app.ld) so they survive firmware upgrades. Every board of a fleet needs the same
 * frequency: nothing on the air tells a board it is on another channel. */
#ifndef RADIO_CFG_H
#define RADIO_CFG_H

#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t freq_khz;
    int8_t   power_dbm; /* conducted, at the SX1262 */
    uint8_t  lbt;       /* 1: RSSI listen-before-talk (radio_lbt_listen) before every TX */
} radio_cfg_t;

#define RADIO_CFG_DEFAULT { RADIO_DEF_FREQ_KHZ, RADIO_DEF_POWER_DBM, RADIO_DEF_LBT }

/* 1 when every field is within what the Ra-01SH is rated for (config.h). */
static inline int radio_cfg_valid(const radio_cfg_t *c)
{
    return c->freq_khz >= RADIO_FREQ_MIN_KHZ && c->freq_khz <= RADIO_FREQ_MAX_KHZ &&
           c->power_dbm >= RADIO_POWER_MIN_DBM && c->power_dbm <= RADIO_POWER_MAX_DBM && c->lbt <= 1u;
}

/* The stored settings, or the defaults when none are stored (or the page is corrupt). */
void radio_cfg_load(radio_cfg_t *out);

/* Erase, write and verify the page: 0 or <0. Blocks (~90 ms page erase): provisioning path only. */
int radio_cfg_write(const radio_cfg_t *c);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_CFG_H */
