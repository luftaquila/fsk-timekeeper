/* C API over RadioLib's SX1262: every call bounded, failures counted in errlog. Main loop only. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#include "radio_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings for radio_begin() and radio_lbt_enabled() (default: config.h); marks init pending. */
void radio_set_config(const radio_cfg_t *cfg);
const radio_cfg_t *radio_config(void);

/* Reset (NRST) and configure the radio; also the recovery path. A silent radio costs ~0.5 s here.
 * 0 or a RadioLib error code; initialization stays pending until radio_start_rx() succeeds. */
int radio_begin(void);

/* Absolute TIMER2 us limit (< 2^31 us ahead) for HAL waits; radio_rx_settle(), radio_cad() and
 * radio_lbt_listen() take it as an access deadline between steps, each SPI command getting its own
 * TX_SETUP_MS; radio_tx_start() sets its own. A timed-out command holds NRST low until recovery. */
void radio_limit(uint32_t end_us);
void radio_unlimit(void);
/* Async TX, setup <= TX_SETUP_MS, done + RX re-arm <= end_us; poll: 0 pending, 1 done, -1 failed */
int radio_tx_start(const uint8_t *data, int len, uint32_t end_us);
int radio_tx_poll(void);
/* Hold the chip in reset immediately; a full begin + RX is required afterwards. */
void radio_tx_abort(void);

/* Continuous RX, 0 on success; DIO1 = RxDone, preamble/header latched for radio_rx_settle(). */
int radio_start_rx(void);

/* Park the radio (a stopped master): standby on the RC oscillator, XOSC off. */
int radio_standby(void);

/* Read a packet and re-arm RX: > 0 length (rssi/snr may be NULL), 0 nothing, < 0 receive error. */
int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr);

/* Wait out a reception: no header after header_wait_ms is noise (flags stay latched until the next
 * CAD, TX or RX re-arm); a packet still arriving after max_ms or the access deadline is BUSY. */
#define RADIO_RX_IDLE   0 /* nothing on air for us */
#define RADIO_RX_PACKET 1 /* a packet is ready for radio_receive() */
#define RADIO_RX_BUSY   2 /* a packet is still arriving */
int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms);

/* CAD: an elapsed access deadline is BUSY (not a failure); an error is never CLEAR. BUSY/ERROR
 * try to re-arm RX; CLEAR leaves the radio out of RX, so transmit or re-arm RX before listening. */
#define RADIO_CAD_ERROR (-1)
#define RADIO_CAD_CLEAR 0
#define RADIO_CAD_BUSY  1
int radio_cad(void);

/* RSSI LBT (radio in RX; caller checks radio_lbt_enabled()): QUIET only if every RSSI read over
 * LBT_LISTEN_MS stayed <= LBT_THRESHOLD_DBM. A failed read or elapsed access deadline is LOUD. */
#define RADIO_LBT_QUIET 0
#define RADIO_LBT_LOUD  1
int radio_lbt_enabled(void);
int radio_lbt_listen(void);

/* Init/RX startup pending (SPI successes do not clear it), or consecutive SPI no-responses or
 * failed CADs reached RADIO_NORESP_RESET. */
int radio_needs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
