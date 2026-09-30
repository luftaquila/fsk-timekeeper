/* Board services: clocks, timebase, LED, power gate (raw MDK registers). */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clear UICR.NFCPINS once (resets), relocate the vector table, start HFXO,
 * start TIMER2, drive EXT_POWER high, set up the LED. Call once at the top of main(). */
void board_init(void);

/* Non-zero only while the 16 MHz clock is sourced from HFXO. Timing work must
 * fail closed when this is false. */
int board_hfclk_xtal(void);

/* Keep HFXO running: when HFCLK is not on the crystal, request HFXO again
 * (non-blocking) and count the restart. Call every main-loop pass in both roles. */
void board_hfxo_service(void);

/* HFXO stops board_hfxo_service() has seen since boot. The restart may finish
 * within the same main-loop pass, before a later board_hfclk_xtal() check:
 * timing code compares this count as well. */
uint32_t board_hfxo_stops(void);

/* EXT_POWER gate (P0.13) — enables the 12 V boost / sensor rails. */
void board_ext_power_on(void);

void board_led_off(void);
void board_led_toggle(void);

/* Free-running 1 MHz TIMER2 (32-bit, wraps every ~71.6 min) and a 32-bit ms
 * count extended through that wrap. Main-loop context only (not ISR safe);
 * board_millis() must run at least once per micros wrap. */
uint32_t board_micros(void);
uint32_t board_millis(void);

/* Busy-wait on TIMER2. */
void board_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
