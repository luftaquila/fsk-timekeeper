/* Stage-3 hardware timestamp capture (DESIGN.md §2.4, §8).
 *
 * TIMER1 free-runs at 16 MHz (62.5 ns) as the common timebase, extended to
 * 64 bits in software (the raw 32-bit counter wraps every ~268 s, too short for
 * a full run). DIO1 (Tx/RxDone) and SENSOR edges are latched into TIMER1 CC
 * registers by GPIOTE->PPI with zero CPU latency; the 32-bit capture is widened
 * to 64 bits against the current time. capture_now64() must be called often
 * enough to never miss a wrap (the per-second beacon loop guarantees this).
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

void capture_init(void);

/* Current 64-bit 16 MHz tick. Also advances the wrap accounting — call it
 * regularly (at least once per ~268 s). */
uint64_t capture_now64(void);

/* If a DIO1 rising edge (Tx/RxDone) was latched since the last call, widen it to
 * 64 bits into *tick and return 1; else 0. */
int capture_dio1_get(uint64_t *tick);

/* Pop the oldest SENSOR falling edge from the ISR-backed ring buffer. */
int capture_sensor_get(uint64_t *tick, uint32_t *seq, int *clock_xtal);
int capture_sensor_loss(uint64_t *first_tick, uint64_t *last_tick, uint32_t *first_seq, uint32_t *last_seq);
int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq);

/* Lifetime diagnostic only; loss ranges carry the affected capture boundaries. */
uint16_t capture_sensor_overflow(void);

/* Master-only USB clock monitor. PPI captures each USBD SOF into a spare TIMER1
 * CC register; samples are diagnostic and never alter the event timebase. */
void capture_usb_sof_enable(void);
int capture_usb_sof_sample(uint64_t *tick, uint16_t *frame);

#endif /* CAPTURE_H */
