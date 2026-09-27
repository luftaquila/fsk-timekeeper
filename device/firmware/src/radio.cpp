#include <RadioLib.h>

#include "radio_hal.h"
#include "radio.h"
#include "gpio.h"

extern "C" {
#include "config.h"
}

/* Statically allocated — no heap. Constructors run from __libc_init_array
 * before main(); hardware init happens in radio_begin() -> radio.begin(). */
static NrfHal hal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
static Module mod(&hal, PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY);
static SX1262 radio(&mod);

extern "C" int radio_begin(float freq_mhz)
{
    /* begin() occasionally fails with garbled SPI readback on the hand-built
     * board; retry a few times (each begin() re-resets the radio). */
    int state = RADIOLIB_ERR_NONE;
    for (int attempt = 0; attempt < 5; attempt++) {
        state = radio.begin(freq_mhz, LORA_BW_KHZ, LORA_SF, LORA_CR,
                            LORA_SYNCWORD, LORA_POWER_DBM, LORA_PREAMBLE,
                            LORA_TCXO_V, false);
        if (state == RADIOLIB_ERR_NONE) {
            break;
        }
    }
    if (state != RADIOLIB_ERR_NONE) {
        return state;
    }

    /* Ra-01SH RF switch (TXEN/RXEN) — required for any TX/RX (DESIGN §3/§8). */
    radio.setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);
    return RADIOLIB_ERR_NONE;
}

extern "C" int radio_transmit(const uint8_t *data, int len)
{
    return radio.transmit(const_cast<uint8_t *>(data), (size_t)len);
}

extern "C" int radio_start_rx(void)
{
    return radio.startReceive();
}

extern "C" int radio_lbt_clear(void)
{
    /* CAD-based listen-before-talk (DESIGN §2.8): clear unless a LoRa preamble is
     * detected on-channel. This is the exact channel check the pre-LBT firmware
     * used for EVENT/STATUS backoff — proven, no false-busy — unlike the
     * instantaneous-RSSI energy detect which mis-read right after startReceive()
     * (~0 dBm default) and starved the master's beacon, wrecking sensor sync.
     * Anything other than a positive detection (including a scan error) counts as
     * clear, so the sync-critical beacon is never starved by a flaky scan. */
    return radio.scanChannel() != RADIOLIB_LORA_DETECTED;
}

extern "C" int radio_receive(uint8_t *buf, int maxlen)
{
    /* DIO1 is mapped to RxDone (+ error/timeout) by startReceive(); we poll it
     * since attachInterrupt is a stub in this stage. */
    if (gpio_read(PIN_LORA_DIO1) == 0) {
        return 0;
    }
    size_t len = radio.getPacketLength();
    if (len > (size_t)maxlen) {
        len = (size_t)maxlen;
    }
    int16_t state = radio.readData(buf, len);
    radio.startReceive(); /* re-arm */
    return (state == RADIOLIB_ERR_NONE) ? (int)len : -1;
}

extern "C" int radio_receive_q(uint8_t *buf, int maxlen, float *rssi, float *snr)
{
    if (gpio_read(PIN_LORA_DIO1) == 0) {
        return 0;
    }
    size_t len = radio.getPacketLength();
    if (len > (size_t)maxlen) {
        len = (size_t)maxlen;
    }
    int16_t state = radio.readData(buf, len);
    /* Sample link quality of the just-read packet BEFORE re-arming. */
    if (rssi) { *rssi = radio.getRSSI(true); }
    if (snr)  { *snr = radio.getSNR(); }
    radio.startReceive(); /* re-arm */
    return (state == RADIOLIB_ERR_NONE) ? (int)len : -1;
}
