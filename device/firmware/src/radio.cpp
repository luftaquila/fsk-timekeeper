#include <string.h>

#include <RadioLib.h>

#include "radio_hal.h"
#include "radio.h"
#include "gpio.h"
#include "board.h"
#include "errlog.h"

extern "C" {
#include "config.h"
}

/* Statically allocated; hardware init happens in radio_begin(). */
static NrfHal hal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_BUSY);
static Module mod(&hal, PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY);
static SX1262 radio(&mod);

/* RX latches preamble/header detections so an in-progress reception is visible
 * before a CAD; DIO1 stays on RxDone. */
#define RX_IRQ_FLAGS (RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED))

static unsigned s_noresp;
static unsigned s_cad_fail;

/* Count SPI no-responses (BUSY stuck, SPIM hung, a status byte nobody drove);
 * any answered command clears the streak. */
static int16_t track(int16_t state)
{
    bool spim_hung = hal.takeSpiTimeout();
    if (spim_hung || state == RADIOLIB_ERR_SPI_CMD_TIMEOUT || state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
        s_noresp++;
        el_note(EL_SPI_TIMEOUT);
    } else {
        s_noresp = 0;
    }
    return state;
}

/* SX126x ReadRegister frame: opcode, 16-bit address, a status byte, then the data.
 * RadioLib's Module switches to this framing only inside begin(), so the probe
 * sends it itself. */
static bool read_version(char version[16])
{
    uint32_t t0 = board_millis();
    while (gpio_read(PIN_LORA_BUSY)) {
        if ((uint32_t)(board_millis() - t0) >= RADIO_SPI_TIMEOUT_MS) { return false; }
    }
    uint8_t out[4 + 16] = { RADIOLIB_SX126X_CMD_READ_REGISTER,
                            (uint8_t)(RADIOLIB_SX126X_REG_VERSION_STRING >> 8),
                            (uint8_t)(RADIOLIB_SX126X_REG_VERSION_STRING & 0xFFu),
                            RADIOLIB_SX126X_CMD_NOP };
    uint8_t in[sizeof(out)] = { 0 };
    gpio_clear(PIN_LORA_NSS);
    hal.spiTransfer(out, sizeof(out), in);
    gpio_set(PIN_LORA_NSS);
    memcpy(version, &in[4], 16);
    return true;
}

/* Pulse NRST and wait for the version string: a radio that never answers would
 * keep begin() in findChip (10 resets, each retrying standby for 1 s). */
static bool radio_answers(void)
{
    mod.init(); /* SPI and NSS; begin() repeats it */
    gpio_cfg_input(PIN_LORA_BUSY);
    radio.reset(false);
    uint32_t t0 = board_millis();
    for (;;) {
        char version[16] = { 0 };
        if (read_version(version) && strncmp(version, RADIOLIB_SX1262_CHIP_TYPE, 6) == 0) { return true; }
        if ((uint32_t)(board_millis() - t0) >= RADIO_PROBE_MS) { return false; }
        board_delay_ms(1);
    }
}

/* Keep the TCXO running in standby, and fall back to that standby after RX and
 * TX: RX and CAD then start without the 5 ms TCXO wait. */
static int16_t standby_on_tcxo(void)
{
    radio.standbyXOSC = true;
    const uint8_t fallback = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_XOSC;
    int16_t state = mod.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_RX_TX_FALLBACK_MODE, &fallback, 1);
    if (state == RADIOLIB_ERR_NONE) {
        state = radio.standby();
    }
    return state;
}

extern "C" int radio_begin(void)
{
    mod.spiConfig.timeout = RADIO_SPI_TIMEOUT_MS;
    /* begin() occasionally fails with garbled SPI readback on the hand-built
     * board; each attempt resets the radio again. */
    int16_t state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    for (int attempt = 0; attempt < 5; attempt++) {
        hal.restartHangCheck();
        if (!radio_answers()) {
            continue;
        }
        /* begin() runs on the RC standby (the TCXO answers to DIO3 only once begin() has
         * configured it) and makes DIO2 drive the RF switch: the Ra-01SH ties TXEN to DIO2
         * and leaves RXEN open, so neither pin is wired. */
        radio.standbyXOSC = false;
        state = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                            LORA_SYNCWORD, LORA_POWER_DBM, LORA_PREAMBLE,
                            LORA_TCXO_V, false);
        if (state == RADIOLIB_ERR_NONE) {
            state = standby_on_tcxo();
        }
        if (state == RADIOLIB_ERR_NONE) {
            break;
        }
    }
    (void)hal.takeSpiTimeout();
    if (state != RADIOLIB_ERR_NONE) {
        return state;
    }
    s_noresp = 0;
    s_cad_fail = 0;
    return RADIOLIB_ERR_NONE;
}

extern "C" int radio_transmit(const uint8_t *data, int len)
{
    hal.restartHangCheck();
    int16_t state = track(radio.transmit(data, (size_t)len));
    if (state != RADIOLIB_ERR_NONE) { el_note(EL_TX_FAIL); }
    return state;
}

extern "C" int radio_start_rx(void)
{
    hal.restartHangCheck();
    int16_t state = track(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RX_IRQ_FLAGS,
                                             RADIOLIB_IRQ_RX_DEFAULT_MASK, 0));
    if (state != RADIOLIB_ERR_NONE) { el_note(EL_RX_FAIL); }
    return state;
}

extern "C" int radio_standby(void)
{
    hal.restartHangCheck();
    return track(radio.standby(RADIOLIB_SX126X_STANDBY_RC)); /* parked: the TCXO stops too */
}

extern "C" int radio_receive(uint8_t *buf, int maxlen, float *rssi, float *snr)
{
    hal.restartHangCheck();
    if (gpio_read(PIN_LORA_DIO1) == 0) {
        return 0;
    }
    size_t len = radio.getPacketLength();
    /* A failed length read gives 0, and readData(buf, 0) takes the whole packet
     * whatever its size. */
    if (len == 0) {
        radio_start_rx();
        el_note(EL_RX_FAIL);
        return -1;
    }
    if (len > (size_t)maxlen) {
        len = (size_t)maxlen;
    }
    int16_t state = track(radio.readData(buf, len));
    /* Link quality of this packet, before re-arming. */
    if (rssi) { *rssi = radio.getRSSI(true); }
    if (snr)  { *snr = radio.getSNR(); }
    radio_start_rx();
    if (state != RADIOLIB_ERR_NONE) {
        el_note(EL_RX_FAIL);
        return -1;
    }
    return (int)len;
}

extern "C" int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms)
{
    hal.restartHangCheck();
    uint32_t t0 = board_millis();
    for (;;) {
        if (gpio_read(PIN_LORA_DIO1)) {
            return RADIO_RX_PACKET;
        }
        uint32_t irq = radio.getIrqFlags();
        uint32_t waited = board_millis() - t0;
        /* A valid header means a packet is arriving, whatever older header error
         * is latched beside it. A header error or a preamble that led nowhere
         * stays latched: the caller's next CAD or TX clears the IRQ status. */
        if (irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) {
            if (waited >= max_ms) { return RADIO_RX_BUSY; }
        } else if ((irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) || !(irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) ||
                   waited >= header_wait_ms) {
            return RADIO_RX_IDLE;
        }
        board_delay_ms(1);
    }
}

/* A failed start or a CAD that never completes counts towards a reset. The
 * start fails with WRONG_MODEM once the radio has reset itself: back in GFSK,
 * it still transmits and receives, so only this sees it. */
extern "C" int radio_cad(void)
{
    hal.restartHangCheck();
    if (track(radio.startChannelScan()) != RADIOLIB_ERR_NONE) {
        el_note(EL_CAD_TIMEOUT);
        s_cad_fail++;
        radio_start_rx();
        return 0;
    }
    uint32_t t0 = board_millis();
    while (gpio_read(PIN_LORA_DIO1) == 0) {
        if ((uint32_t)(board_millis() - t0) >= CAD_TIMEOUT_MS) {
            el_note(EL_CAD_TIMEOUT);
            s_cad_fail++;
            radio.standby();
            radio_start_rx();
            return 0;
        }
    }
    s_cad_fail = 0;
    if (radio.getChannelScanResult() == RADIOLIB_LORA_DETECTED) {
        radio_start_rx();
        return 1;
    }
    return 0;
}

extern "C" int radio_needs_reset(void)
{
    return s_noresp >= RADIO_NORESP_RESET || s_cad_fail >= RADIO_NORESP_RESET;
}
