/* C-callable LoRa radio bring-up API (wraps the C++ RadioLib SX1262). */
#ifndef RADIO_H
#define RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the SX1262 on the given frequency (MHz) with the DESIGN §2
 * parameters. Returns 0 on success, else the (negative) RadioLib error code. */
int radio_begin(float freq_mhz);

#include <stdint.h>

/* Blocking transmit of len bytes. Returns 0 on success. */
int radio_transmit(const uint8_t *data, int len);

/* Put the radio into continuous receive. Returns 0 on success. */
int radio_start_rx(void);

/* Non-blocking RX poll: >0 = bytes read into buf (and re-armed), 0 = nothing
 * yet, <0 = receive error. */
int radio_receive(uint8_t *buf, int maxlen);

/* Same as radio_receive, but also returns the last packet's link quality
 * (RSSI dBm, SNR dB) sampled before the radio is re-armed. Used by the master
 * for per-sensor diagnostics. The rssi/snr outputs are only meaningful when the
 * return value is > 0. */
int radio_receive_q(uint8_t *buf, int maxlen, float *rssi, float *snr);

/* Listen-before-talk energy detect (DESIGN §2.8, KR920 coexistence): senses the
 * channel for LBT_SENSE_MS and returns non-zero only if it stayed clear (peak
 * RSSI below LBT_RSSI_DBM) the whole time. Call immediately before every
 * transmit; transmit only when this returns non-zero. Leaves the radio in RX. */
int radio_lbt_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
