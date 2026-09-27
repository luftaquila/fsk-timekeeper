/* Board services for bring-up.
 *
 * Stage 0/1 stay on raw MDK registers via gpio.h (no nrfx drivers yet) so the
 * surface stays minimal. nrfx HAL/drivers arrive with the radio/capture.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Relocate the vector table to the app base (0x26000), drive EXT_POWER (P0.13)
 * HIGH to enable the external VCC/12V rail (DESIGN.md §8), and bring up the LED.
 * Call once at the top of main(). */
void board_init(void);

/* Non-zero only while the 16 MHz peripheral clock is sourced from HFXO. Radio
 * timing and event capture must fail closed when this is false. */
int board_hfclk_xtal(void);

/* EXT_POWER gate (P0.13) — enables the 12V boost / sensor & light rails. */
void board_ext_power_on(void);
void board_ext_power_off(void);

void board_led_on(void);
void board_led_off(void);
void board_led_toggle(void);
void board_led_write(int on);

/* Free-running 1 MHz timebase (TIMER2), started in board_init(). Shared by the
 * RadioLib HAL and the main loop for non-blocking timing. 32-bit, wraps ~71 min. */
uint32_t board_micros(void);
uint32_t board_millis(void);

/* Busy-wait using the timebase. */
void board_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
