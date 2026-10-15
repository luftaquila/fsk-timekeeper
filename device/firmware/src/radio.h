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

/* Blocking transmit (<= ~5x airtime). Leaves the radio in standby. 0 on success. */
int radio_transmit(const uint8_t *data, int len);

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
 * after max_ms returns RADIO_RX_BUSY. */
#define RADIO_RX_IDLE   0 /* nothing on air for us */
#define RADIO_RX_PACKET 1 /* a packet is ready for radio_receive() */
#define RADIO_RX_BUSY   2 /* a packet is still arriving */
int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms);

/* CAD result; errors must never be interpreted as a clear channel. */
#define RADIO_CAD_ERROR (-1)
#define RADIO_CAD_CLEAR 0
#define RADIO_CAD_BUSY  1
int radio_cad(void);

/* RSSI listen-before-talk, when the settings enable it: receive for LBT_LISTEN_MS
 * (the radio must be in RX) and report QUIET only when every instantaneous RSSI
 * stayed at or below LBT_THRESHOLD_DBM. A failed read is LOUD: errors must never
 * pass for a quiet channel. */
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
