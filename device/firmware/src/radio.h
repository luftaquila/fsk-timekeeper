/* C API over the RadioLib SX1262 (Ra-01SH). Every call is bounded; failures are
 * counted in errlog. Main-loop context only. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset (NRST) and configure the radio with the config.h parameters. Also the
 * recovery path. A radio that does not answer after NRST costs ~0.5 s here.
 * Returns 0 on success, else the RadioLib error code. */
int radio_begin(void);

/* Blocking transmit (<= ~5x airtime). Leaves the radio in standby. 0 on success. */
int radio_transmit(const uint8_t *data, int len);

/* Continuous receive; DIO1 = RxDone, preamble/header detections latched in the
 * IRQ status for radio_rx_settle(). 0 on success. */
int radio_start_rx(void);

/* Park the radio (a stopped master): standby on the RC oscillator, TCXO off. */
int radio_standby(void);

/* Read a received packet: > 0 = length (RSSI/SNR filled when non-NULL; either
 * may be NULL), 0 = nothing, < 0 = receive error. Re-arms reception. */
int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr);

/* Wait out a reception in progress. A preamble with no header after
 * header_wait_ms is noise (it may be a stale detection: the flags stay latched
 * until the next CAD or TX); a packet still arriving after max_ms returns
 * RADIO_RX_BUSY. */
#define RADIO_RX_IDLE   0 /* nothing on air for us */
#define RADIO_RX_PACKET 1 /* a packet is ready for radio_receive() */
#define RADIO_RX_BUSY   2 /* a packet is still arriving */
int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms);

/* Listen-before-talk channel activity detection, bounded by CAD_TIMEOUT_MS.
 * Returns 1 = LoRa activity (the radio is back in receive), 0 = clear (standby,
 * ready to transmit). A scan error or timeout counts as clear so the beacon is
 * never starved by a flaky scan. */
int radio_cad(void);

/* Consecutive SPI no-responses or failed CADs reached RADIO_NORESP_RESET. */
int radio_needs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
