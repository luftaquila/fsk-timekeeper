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
static radio_cfg_t s_cfg = RADIO_CFG_DEFAULT;
/* Initialization is complete only after the configured receiver starts. Normal
 * RX re-arms keep their existing SPI failure threshold. */
static bool s_init_pending;
static bool s_configured;
static bool s_tx_active;

static void hard_stop(void)
{
    /* No SPI or waits here: a stuck peripheral must not keep transmitting. */
    gpio_set(PIN_LORA_NSS);
    gpio_clear(PIN_LORA_NRST);
    s_init_pending = true;
    s_configured = false;
    s_tx_active = false;
}

extern "C" void radio_tx_abort(void) { hard_stop(); hal.unlimit(); }
extern "C" void radio_limit(uint32_t end_us) { hal.limit(end_us); }
extern "C" void radio_unlimit(void) { hal.unlimit(); }
/* The wire has one RX bit, but starting RX and reading a packet recover
 * independently. Neither success is evidence that the other path recovered. */
static unsigned s_rx_faults;
enum { RX_START_FAULT = 1u, RX_READ_FAULT = 2u };
static void rx_result(unsigned source, bool ok)
{
    if (!ok) {
        s_rx_faults |= source;
        el_note(EL_RX_FAIL);
    } else {
        s_rx_faults &= ~source;
        if (!s_rx_faults) { el_radio_clear(EL_RX_FAIL); }
    }
}

/* Count SPI no-responses (BUSY stuck, SPIM hung, a status byte nobody drove);
 * any answered command clears the streak. */
static int16_t track(int16_t state)
{
    if (hal.deadlineHit()) { hard_stop(); state = RADIOLIB_ERR_SPI_CMD_TIMEOUT; }
    bool spim_hung = hal.takeSpiTimeout();
    if (spim_hung) { state = RADIOLIB_ERR_SPI_CMD_TIMEOUT; }
    if (spim_hung || state == RADIOLIB_ERR_SPI_CMD_TIMEOUT || state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
        s_noresp++;
        el_note(EL_SPI_TIMEOUT);
    } else {
        s_noresp = 0;
        if (state == RADIOLIB_ERR_NONE) { el_radio_clear(EL_SPI_TIMEOUT); }
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

/* Read errors even when an initialization command failed, before the next
 * reset loses the oscillator/calibration evidence. A failed read is not zero. */
static int16_t check_device_errors(void)
{
    uint8_t bytes[2] = { 0 };
    int16_t state = mod.SPIreadStream(RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS, bytes, sizeof(bytes));
    if (hal.takeSpiTimeout()) { return RADIOLIB_ERR_SPI_CMD_TIMEOUT; }
    if (state != RADIOLIB_ERR_NONE) { return state; }
    const uint16_t errors = (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
    const uint16_t clock_errors = RADIOLIB_SX126X_XOSC_START_ERR | RADIOLIB_SX126X_PLL_LOCK_ERR;
    if (errors & clock_errors) { el_note(EL_RADIO_CLOCK); }
    if (errors & ~clock_errors) { el_note(EL_RADIO_INIT); }
    return errors ? RADIOLIB_ERR_SPI_CMD_FAILED : RADIOLIB_ERR_NONE;
}

/* Verify the actual oscillator/mode, not just command acceptance. RadioLib's
 * getStatus() reads zero data bytes, so read the status byte directly. Only
 * bring-up uses this check; normal RX re-arms retain their timing budget. */
static int16_t verify_radio_mode(uint8_t expected)
{
    uint32_t t0 = board_millis();
    while (gpio_read(PIN_LORA_BUSY)) {
        if ((uint32_t)(board_millis() - t0) >= RADIO_SPI_TIMEOUT_MS) {
            return RADIOLIB_ERR_SPI_CMD_TIMEOUT;
        }
    }
    uint8_t out[2] = { RADIOLIB_SX126X_CMD_GET_STATUS, 0 }, in[2] = { 0 };
    gpio_clear(PIN_LORA_NSS);
    hal.spiTransfer(out, sizeof(out), in);
    gpio_set(PIN_LORA_NSS);
    if (hal.takeSpiTimeout()) { return RADIOLIB_ERR_SPI_CMD_TIMEOUT; }
    if (in[1] == 0 || in[1] == 0xFF) { return RADIOLIB_ERR_CHIP_NOT_FOUND; }
    int16_t state = check_device_errors();
    if (state != RADIOLIB_ERR_NONE) { return state; }
    const uint8_t command = in[1] & 0x0Eu;
    if (command == RADIOLIB_SX126X_STATUS_CMD_TIMEOUT) { return RADIOLIB_ERR_SPI_CMD_TIMEOUT; }
    if (command == RADIOLIB_SX126X_STATUS_CMD_INVALID || command == RADIOLIB_SX126X_STATUS_CMD_FAILED) {
        return RADIOLIB_ERR_SPI_CMD_FAILED;
    }
    if ((in[1] & 0x70u) != expected) {
        el_note(EL_RADIO_INIT);
        return RADIOLIB_ERR_SPI_CMD_FAILED;
    }
    return RADIOLIB_ERR_NONE;
}

/* Keep the crystal oscillator running in standby and after RX/TX, avoiding
 * another oscillator startup before the next RX or CAD. */
static int16_t standby_on_xosc(void)
{
    radio.standbyXOSC = true;
    const uint8_t fallback = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_XOSC;
    int16_t state = mod.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_RX_TX_FALLBACK_MODE, &fallback, 1);
    if (state == RADIOLIB_ERR_NONE) {
        state = radio.standby();
    }
    if (state == RADIOLIB_ERR_NONE) {
        state = verify_radio_mode(RADIOLIB_SX126X_STATUS_MODE_STDBY_XOSC);
    } else {
        (void)check_device_errors();
    }
    return state;
}

extern "C" void radio_set_config(const radio_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_init_pending = true;
    s_configured = false;
}
extern "C" const radio_cfg_t *radio_config(void) { return &s_cfg; }

extern "C" int radio_begin(void)
{
    hal.unlimit();
    s_tx_active = false;
    s_init_pending = true;
    s_configured = false;
    mod.spiConfig.timeout = RADIO_SPI_TIMEOUT_MS;
    /* begin() occasionally fails with garbled SPI readback on the hand-built
     * board; each attempt resets the radio again. */
    int16_t state = RADIOLIB_ERR_CHIP_NOT_FOUND;
    for (int attempt = 0; attempt < 5; attempt++) {
        hal.restartHangCheck();
        if (!radio_answers()) {
            continue;
        }
        /* Begin in RC standby, explicitly using the Ra-01SH's passive crystal.
         * RadioLib also enables the module's internal DIO2 RF switch control. */
        radio.standbyXOSC = false;
        state = radio.begin(s_cfg.freq_khz / 1000.0f, LORA_BW_KHZ, LORA_SF, LORA_CR,
                            LORA_SYNCWORD, s_cfg.power_dbm, LORA_PREAMBLE,
                            LORA_TCXO_V, false);
        if (state == RADIOLIB_ERR_NONE) {
            state = standby_on_xosc();
        } else {
            (void)check_device_errors();
        }
        state = track(state);
        if (state == RADIOLIB_ERR_NONE) {
            break;
        }
    }
    (void)hal.takeSpiTimeout();
    if (state != RADIOLIB_ERR_NONE) {
        el_note(EL_RADIO_INIT);
        return state;
    }
    s_noresp = 0;
    s_cad_fail = 0;
    s_configured = true;
    return RADIOLIB_ERR_NONE;
}

extern "C" int radio_tx_start(const uint8_t *data, int len, uint32_t end_us)
{
    if (s_tx_active || radio_needs_reset() || len <= 0 || len > 128) { return -1; }
    uint32_t now = board_micros();
    if ((int32_t)(end_us - now) < (int32_t)(TX_SETUP_MS * 1000u + LORA_TOA_US((unsigned)len))) { return -1; }
    hal.limit(now + TX_SETUP_MS * 1000u);
    hal.restartHangCheck();
    int16_t state = track(radio.startTransmit(data, (size_t)len));
    if (state != RADIOLIB_ERR_NONE || hal.expired()) {
        hard_stop(); hal.unlimit(); el_note(EL_TX_FAIL); return -1;
    }
    s_tx_active = true;
    hal.limit(end_us);
    return 0;
}

extern "C" int radio_tx_poll(void)
{
    if (!s_tx_active) { return -1; }
    if (hal.expired()) {
        hard_stop(); hal.unlimit(); el_note(EL_TX_FAIL); return -1;
    }
    if (!gpio_read(PIN_LORA_DIO1)) { return 0; }
    int16_t state = track(radio.finishTransmit());
    s_tx_active = false;
    if (state == RADIOLIB_ERR_NONE) { state = (int16_t)radio_start_rx(); }
    bool late = hal.expired();
    if (state != RADIOLIB_ERR_NONE || late) { hard_stop(); el_note(EL_TX_FAIL); }
    else { el_radio_clear(EL_TX_FAIL); }
    hal.unlimit();
    return state == RADIOLIB_ERR_NONE && !late ? 1 : -1;
}

extern "C" int radio_start_rx(void)
{
    hal.restartHangCheck();
    int16_t state = radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RX_IRQ_FLAGS,
                                       RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    if (s_init_pending && s_configured) {
        if (state == RADIOLIB_ERR_NONE) {
            state = verify_radio_mode(RADIOLIB_SX126X_STATUS_MODE_RX);
        } else {
            (void)check_device_errors();
        }
    }
    state = track(state);
    if (state != RADIOLIB_ERR_NONE) { rx_result(RX_START_FAULT, false); }
    else if (s_configured) {
        rx_result(RX_START_FAULT, true);
        if (s_init_pending) {
            el_radio_clear(EL_RADIO_CLOCK);
            el_radio_clear(EL_RADIO_INIT);
        }
        s_init_pending = false;
    }
    return state;
}

extern "C" int radio_standby(void)
{
    hal.restartHangCheck();
    return track(radio.standby(RADIOLIB_SX126X_STANDBY_RC)); /* parked: XOSC stops too */
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
        rx_result(RX_READ_FAULT, false);
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
        rx_result(RX_READ_FAULT, false);
        return -1;
    }
    rx_result(RX_READ_FAULT, true);
    return (int)len;
}

/* The access deadline cancels channel assessment, not a healthy SPI command.
 * Restore it for the caller; each command/re-arm gets a bounded SPI budget. */
struct RadioAccess {
    uint32_t end;
    bool limited;
    RadioAccess() : limited(hal.getLimit(end)) {}
    ~RadioAccess() { if (limited) { hal.limit(end); } else { hal.unlimit(); } }
    bool expired() const { return limited && (int32_t)(board_micros() - end) >= 0; }
};

extern "C" int radio_rx_settle(uint32_t header_wait_ms, uint32_t max_ms)
{
    RadioAccess access;
    hal.restartHangCheck();
    uint32_t t0 = board_millis();
    for (;;) {
        if (access.expired()) { return RADIO_RX_BUSY; }
        if (gpio_read(PIN_LORA_DIO1)) {
            return RADIO_RX_PACKET;
        }
        hal.limit(board_micros() + TX_SETUP_MS * 1000u);
        uint32_t irq = radio.getIrqFlags();
        if (hal.deadlineHit()) { track(RADIOLIB_ERR_SPI_CMD_TIMEOUT); return RADIO_RX_BUSY; }
        if (access.expired()) { return RADIO_RX_BUSY; }
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

static int cad_rearm(int result)
{
    if (s_init_pending && !s_configured) { return RADIO_CAD_ERROR; }
    hal.limit(board_micros() + TX_SETUP_MS * 1000u);
    /* startReceive enters standby before setting RX, also stopping a live CAD. */
    return radio_start_rx() == RADIOLIB_ERR_NONE ? result : RADIO_CAD_ERROR;
}

static int cad_failure(void)
{
    el_note(EL_CAD_TIMEOUT);
    s_cad_fail++;
    return cad_rearm(RADIO_CAD_ERROR);
}

/* A failed start or a CAD that never completes counts towards a reset. The
 * start fails with WRONG_MODEM once the radio has reset itself: back in GFSK,
 * it still transmits and receives, so only this sees it. An elapsed access
 * deadline cancels the transmission, not the health check: the scan is still
 * awaited for up to CAD_DONE_MS, so no slot deadline can hide a hung CAD. */
static_assert(CAD_DONE_MS < CAD_TIMEOUT_MS &&
              SLOT_LATE_MS + CAD_DONE_MS + TX_SETUP_MS + LBT_LISTEN_MS < SLOT_LEN_MS,
              "a scan awaited past its slot deadline ends before the next slot's listen");

extern "C" int radio_cad(void)
{
    RadioAccess access;
    if (access.expired()) { return RADIO_CAD_BUSY; }
    hal.limit(board_micros() + TX_SETUP_MS * 1000u);
    hal.restartHangCheck();
    if (track(radio.startChannelScan()) != RADIOLIB_ERR_NONE) {
        return cad_failure();
    }
    uint32_t t0 = board_micros();
    while (!gpio_read(PIN_LORA_DIO1)) {
        uint32_t limit_ms = access.expired() ? CAD_DONE_MS : CAD_TIMEOUT_MS;
        if (board_micros() - t0 >= limit_ms * 1000u) { return cad_failure(); }
    }
    hal.limit(board_micros() + TX_SETUP_MS * 1000u);
    int16_t result = track(radio.getChannelScanResult());
    if (result != RADIOLIB_LORA_DETECTED && result != RADIOLIB_CHANNEL_FREE) {
        return cad_failure();
    }
    s_cad_fail = 0;
    el_radio_clear(EL_CAD_TIMEOUT);
    if (result == RADIOLIB_LORA_DETECTED || access.expired()) {
        return cad_rearm(RADIO_CAD_BUSY);
    }
    return RADIO_CAD_CLEAR;
}

extern "C" int radio_lbt_enabled(void) { return s_cfg.lbt; }

/* GetRssiInst answers -raw/2 dBm; SPI failures count towards a reset like any command. */
extern "C" int radio_lbt_listen(void)
{
    RadioAccess access;
    hal.restartHangCheck();
    int loud = 0;
    uint32_t t0 = board_micros();
    do {
        if (access.expired()) { return RADIO_LBT_LOUD; }
        hal.limit(board_micros() + TX_SETUP_MS * 1000u);
        uint8_t raw = 0;
        if (track(mod.SPIreadStream(RADIOLIB_SX126X_CMD_GET_RSSI_INST, &raw, 1)) != RADIOLIB_ERR_NONE ||
            raw < -2 * LBT_THRESHOLD_DBM) {
            loud = 1;
        }
        if (access.expired()) { return RADIO_LBT_LOUD; }
    } while ((uint32_t)(board_micros() - t0) < LBT_LISTEN_MS * 1000u);
    return loud ? RADIO_LBT_LOUD : RADIO_LBT_QUIET;
}

extern "C" int radio_needs_reset(void)
{
    return s_init_pending || s_noresp >= RADIO_NORESP_RESET || s_cad_fail >= RADIO_NORESP_RESET;
}
