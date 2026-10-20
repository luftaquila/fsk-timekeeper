/* C API over the RadioLib SX1262 (Ra-01SH). Every call is bounded; failures are
 * counted in errlog. Main-loop context only. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#include "radio_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings that radio_begin() applies from now on and that radio_lbt_enabled()
 * reports; the config.h defaults until set. Marks initialization pending. */
void radio_set_config(const radio_cfg_t *cfg);
const radio_cfg_t *radio_config(void);

/* Reset (NRST) and configure the radio with the config.h parameters and the
 * radio_set_config() settings. Also the recovery path. A radio that does not
 * answer after NRST costs ~0.5 s here. Returns 0 on success, else the RadioLib
 * error code. Initialization remains pending until radio_start_rx() succeeds. */
int radio_begin(void);

/* Absolute TIMER2 microsecond limit, less than 2^31 us ahead. Every HAL wait
 * inherits it; a timed-out command holds NRST low until recovery. */
void radio_limit(uint32_t end_us);
void radio_unlimit(void);
/* Nonblocking TX. Start/setup <= TX_SETUP_MS; completion + RX rearm <= end_us.
 * poll: 0 pending, 1 completed, -1 stopped/failed. */
int radio_tx_start(const uint8_t *data, int len, uint32_t end_us);
int radio_tx_poll(void);
/* Hold the chip in reset immediately; a full begin + RX is required afterwards. */
void radio_tx_abort(void);

/* Continuous receive; DIO1 = RxDone, preamble/header detections latched in the
 * IRQ status for radio_rx_settle(). 0 on success. */
int radio_start_rx(void);

/* Park the radio (a stopped master): standby on the RC oscillator, XOSC off. */
int radio_standby(void);

/* Read a received packet: > 0 = length (RSSI/SNR filled when non-NULL; either
 * may be NULL), 0 = nothing, < 0 = receive error. Re-arms reception. */
int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr);

/* Wait out a reception in progress: a preamble with no header after header_wait_ms is noise
 * (maybe stale: the flags stay latched until the next CAD or TX); a packet still arriving
 * after max_ms or an elapsed access deadline returns RADIO_RX_BUSY. */
#define RADIO_RX_IDLE   0 /* nothing on air for us */
#define RADIO_RX_PACKET 1 /* a packet is ready for radio_receive() */
#define RADIO_RX_BUSY   2 /* a packet is still arriving */
int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms);

/* CAD result; an elapsed access deadline is BUSY, not a device failure.
 * Errors must never be interpreted as a clear channel. BUSY/ERROR
 * attempt to re-arm RX (a restart failure is recorded separately). CLEAR leaves
 * the radio out of RX; the caller must transmit or re-arm RX before listening. */
#define RADIO_CAD_ERROR (-1)
#define RADIO_CAD_CLEAR 0
#define RADIO_CAD_BUSY  1
int radio_cad(void);

/* RSSI listen-before-talk; the caller checks radio_lbt_enabled(). Receive for LBT_LISTEN_MS
 * (the radio must be in RX) and report QUIET only when every instantaneous RSSI
 * stayed at or below LBT_THRESHOLD_DBM. A failed read or elapsed access deadline
 * is LOUD: errors and incomplete checks must never pass for a quiet channel. */
#define RADIO_LBT_QUIET 0
#define RADIO_LBT_LOUD  1
int radio_lbt_enabled(void);
int radio_lbt_listen(void);

/* Initialization/RX startup is pending, or consecutive SPI no-responses or
 * failed CADs reached RADIO_NORESP_RESET. Successful SPI reads do not clear
 * pending initialization. */
int radio_needs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
