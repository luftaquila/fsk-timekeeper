/* TIMER1 hardware timestamps: 16 MHz (62.5 ns), extended to 64 bits in software. GPIOTE->PPI
 * latches DIO1 (Tx/RxDone), SENSOR and GPS PPS into CC with no CPU latency; captures widen against
 * now. capture_now64() must run at least once per ~268 s wrap. Main loop except where noted. */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

/* TIMER1 + DIO1 capture (both roles). */
void capture_init(void);
/* SENSOR input capture into a 64-entry ISR ring (sensor role). */
void capture_sensor_enable(void);
/* GPS PPS capture on its own GPIOTE/PPI channel (master role). */
void capture_pps_enable(void);

/* Raw TIMER1 sample; safe in an ISR, without modifying the main-loop extension. */
uint32_t capture_now32(void);
uint64_t capture_now64(void);

/* Latest DIO1 rising edge since the last call (1), or none (0). */
int capture_dio1_get(uint64_t *tick);

/* Oldest ring edge: seq (counts every edge, lost ones too) and whether HFXO ran at capture. */
int capture_sensor_get(uint64_t *tick, uint32_t *seq, int *clock_xtal);
/* Edges missed by the ISR or dropped by a full ring, as a seq/tick range after every ring edge.
 * Drain capture_sensor_get() first, then this, until both return 0; UNKNOWN: ticks unusable. */
#define CAPTURE_LOSS_KNOWN 1
#define CAPTURE_LOSS_UNKNOWN 2
int capture_sensor_loss(uint64_t *first_tick, uint64_t *last_tick, uint32_t *first_seq, uint32_t *last_seq);
/* Now + last seq; 1 only with nothing pending (ring, loss, latched event): may be a checkpoint. */
int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq);
/* Missed/dropped hardware edges since boot (saturating diagnostic). */
uint16_t capture_sensor_overflow(void);

/* Latest PPS rising edge since the last call (1), or none (0). */
int capture_pps_get(uint64_t *tick);

#endif /* CAPTURE_H */
