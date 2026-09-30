/* Hardware timestamp capture on TIMER1 (DESIGN §2.4, §8).
 *
 * TIMER1 free-runs at 16 MHz (62.5 ns) and is extended to 64 bits in software.
 * GPIOTE -> PPI latches DIO1 (Tx/RxDone), SENSOR and GPS PPS edges into TIMER1
 * CC registers with no CPU latency; a 32-bit capture is widened against the
 * current time. capture_now64() must run at least once per ~268 s wrap.
 * Main-loop context except where noted.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

/* TIMER1 + DIO1 capture (both roles). */
void capture_init(void);
/* SENSOR input capture into a 64-entry ISR ring (sensor role). */
void capture_sensor_enable(void);
/* GPS PPS capture on its own GPIOTE/PPI channel (master role). */
void capture_pps_enable(void);

uint64_t capture_now64(void);

/* Latest DIO1 rising edge since the last call (1), or none (0). */
int capture_dio1_get(uint64_t *tick);

/* Oldest SENSOR edge from the ring: capture seq (counts every edge, lost ones
 * included) and whether HFXO ran when it was taken. */
int capture_sensor_get(uint64_t *tick, uint32_t *seq, int *clock_xtal);
/* Edges the full ring had to drop, as a seq/tick range; it follows every edge
 * still in the ring. Drain with capture_sensor_get() first, then this, until
 * both return 0. */
int capture_sensor_loss(uint64_t *first_tick, uint64_t *last_tick, uint32_t *first_seq, uint32_t *last_seq);
/* Now and the seq of the last edge; returns 1 only when no edge is pending
 * anywhere (ring, loss, latched event), i.e. the pair may be a checkpoint. */
int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq);
/* Ring overflows since boot (diagnostic). */
uint16_t capture_sensor_overflow(void);

/* Latest PPS rising edge since the last call (1), or none (0). */
int capture_pps_get(uint64_t *tick);

#endif /* CAPTURE_H */
