#ifndef MAC_RADIO_H
#define MAC_RADIO_H
#include "board.h"
#include "capture.h"
#include "config.h"
#include "radio.h"

static inline uint32_t mac_deadline_us(uint64_t tick)
{
    uint64_t now = capture_now64();
    return board_micros() + (uint32_t)(tick > now ? (tick - now) / (TICKS_PER_MS / 1000u) : 0);
}
static inline void mac_radio_limit(uint64_t tick) { radio_limit(mac_deadline_us(tick)); }

#endif
