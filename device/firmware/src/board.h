/* Board services: clocks, timebase, LED, power gate (raw MDK registers). */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NFC pins (may reset once), vector table, HFXO, TIMER2, EXT_POWER, LED. Once, at top of main(). */
void board_init(void);

/* Non-zero only while the 16 MHz clock runs from HFXO; timing work must fail closed otherwise. */
int board_hfclk_xtal(void);

/* Off the crystal: re-request HFXO (non-blocking), count the stop. Call every pass, both roles. */
void board_hfxo_service(void);

/* HFXO stops since boot: a restart may end before a later board_hfclk_xtal(), so check this too. */
uint32_t board_hfxo_stops(void);

/* EXT_POWER (P0.13): module VCC (3V3) for radio, GPS, sensor pull-up; 12 V boost is battery-fed. */
void board_ext_power_on(void);

void board_led_off(void);
void board_led_toggle(void);

/* Free-running 1 MHz TIMER2 (wraps ~71.6 min) and a 32-bit ms count extended through that wrap.
 * Main loop only (not ISR safe); board_millis() must run at least once per micros wrap. */
uint32_t board_micros(void);
uint32_t board_millis(void);

/* Busy-wait on TIMER2. */
void board_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
