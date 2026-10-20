/* radio.cpp with the vendor RadioLib against a fake SX1262 behind a fake HAL: the
 * bytes the probe and begin() send, and the chip state they leave. Every
 * scenario runs in a new process, as on MCU boot. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/radio.cpp"
#include "../src/protocol.h"

NRF_GPIO_Type test_gpio[2];

/* ---- clock --------------------------------------------------------------- */
static uint64_t now_us;
static uint64_t cad_done_us;
static uint32_t cad_delay_us;
static void cad_clock(void);

extern "C" uint32_t board_micros(void) { ++now_us; cad_clock(); return (uint32_t)now_us; }
/* Every poll moves time on, so polling loops end as they do on hardware. */
extern "C" uint32_t board_millis(void) { now_us += 1u; cad_clock(); return (uint32_t)(now_us / 1000u); }
extern "C" void board_delay_ms(uint32_t ms) { now_us += (uint64_t)ms * 1000u; }

static unsigned notes[EL_COUNT];
static uint32_t active_faults;
extern "C" void el_note(el_code_t code) { notes[code]++; active_faults |= (uint32_t)(1u << code); }
extern "C" void el_radio_clear(el_code_t code) { active_faults &= (uint32_t)~(1u << code); }

/* ---- fake SX1262 ----------------------------------------------------------- */
typedef std::vector<uint8_t> frame_t;

static struct {
    bool answers = true;        /* drives MISO; a dead radio reads 0xFF */
    bool busy = false;          /* BUSY stuck high */
    bool reject_rx = false;     /* an answered SetRx failure, not a SPI timeout */
    bool stuck_xosc = false, stuck_rx = false, reject_errors = false;
    uint16_t device_errors = 0, irq_flags = 0;
    uint8_t reg[0x10000];
    uint8_t buf[256];
    uint8_t packet_type = 0;    /* GFSK after reset */
    uint8_t mode = RADIOLIB_SX126X_STATUS_MODE_STDBY_RC;
    uint8_t fallback = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_RC;
    std::vector<uint8_t> rx_lengths; /* answers of GetRxBufferStatus, last one repeats */
    uint8_t rssi_raw = 200;     /* GetRssiInst: -raw/2 dBm */
    std::vector<frame_t> frames;
} chip;

static void chip_reset(void)
{
    memset(chip.reg, 0, sizeof(chip.reg));
    memcpy(&chip.reg[RADIOLIB_SX126X_REG_VERSION_STRING], "SX1261 V2D 2D02", 16);
    chip.packet_type = 0;
    chip.mode = RADIOLIB_SX126X_STATUS_MODE_STDBY_RC;
    chip.fallback = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_RC;
}

static void set_line(uint32_t pin, bool high)
{
    uint32_t bit = 1u << (pin & 31u);
    NRF_GPIO_Type *port = pin < 32u ? NRF_P0 : NRF_P1;
    port->IN = high ? (port->IN | bit) : (port->IN & ~bit);
}

static void cad_clock(void)
{
    if (cad_done_us && now_us >= cad_done_us) {
        cad_done_us = 0;
        chip.irq_flags = RADIOLIB_SX126X_IRQ_CAD_DONE;
        set_line(PIN_LORA_DIO1, true);
    }
}

/* Command status 0x2 (processed) in the current chip mode. */
static uint8_t chip_status(void) { return (uint8_t)(chip.mode | 0x02u); }

static void chip_transfer(const uint8_t *out, uint8_t *in, size_t len)
{
    chip.frames.emplace_back(out, out + len);
    if (!chip.answers) { memset(in, 0xFF, len); return; }
    memset(in, chip_status(), len);
    if (chip.reject_rx && out[0] == RADIOLIB_SX126X_CMD_SET_RX) {
        memset(in, RADIOLIB_SX126X_STATUS_CMD_INVALID, len);
        return;
    }
    if (chip.reject_errors && out[0] == RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS) {
        memset(in, RADIOLIB_SX126X_STATUS_CMD_INVALID, len);
        return;
    }
    uint16_t addr = len >= 3 ? (uint16_t)(out[1] << 8 | out[2]) : 0;
    switch (out[0]) {
    case RADIOLIB_SX126X_CMD_SET_CAD:
        if (cad_delay_us) {
            cad_done_us = now_us + cad_delay_us;
            chip.irq_flags = 0;
            set_line(PIN_LORA_DIO1, false);
        }
        break;
    case RADIOLIB_SX126X_CMD_READ_REGISTER:
        for (size_t i = 4; i < len; i++) { in[i] = chip.reg[(uint16_t)(addr + i - 4)]; }
        break;
    case RADIOLIB_SX126X_CMD_WRITE_REGISTER:
        for (size_t i = 3; i < len; i++) { chip.reg[(uint16_t)(addr + i - 3)] = out[i]; }
        break;
    case RADIOLIB_SX126X_CMD_READ_BUFFER:
        for (size_t i = 3; i < len; i++) { in[i] = chip.buf[(uint8_t)(out[1] + i - 3)]; }
        break;
    case RADIOLIB_SX126X_CMD_WRITE_BUFFER:
        for (size_t i = 2; i < len; i++) { chip.buf[(uint8_t)(out[1] + i - 2)] = out[i]; }
        break;
    case RADIOLIB_SX126X_CMD_SET_PACKET_TYPE: chip.packet_type = out[1]; break;
    case RADIOLIB_SX126X_CMD_GET_PACKET_TYPE: if (len > 2) { in[2] = chip.packet_type; } break;
    case RADIOLIB_SX126X_CMD_GET_RSSI_INST: if (len > 2) { in[2] = chip.rssi_raw; } break;
    case RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS:
        if (len > 3) { in[2] = chip.device_errors >> 8; in[3] = chip.device_errors & 0xFF; }
        break;
    case RADIOLIB_SX126X_CMD_GET_IRQ_STATUS:
        if (len > 3) { in[2] = chip.irq_flags >> 8; in[3] = chip.irq_flags & 0xFF; }
        break;
    case RADIOLIB_SX126X_CMD_GET_RX_BUFFER_STATUS:
        if (len > 3) {
            in[2] = chip.rx_lengths.empty() ? 0 : chip.rx_lengths.front();
            in[3] = 0;
            if (chip.rx_lengths.size() > 1) { chip.rx_lengths.erase(chip.rx_lengths.begin()); }
        }
        break;
    case RADIOLIB_SX126X_CMD_SET_STANDBY:
        cad_done_us = 0;
        chip.mode = out[1] == RADIOLIB_SX126X_STANDBY_XOSC && !chip.stuck_xosc ? RADIOLIB_SX126X_STATUS_MODE_STDBY_XOSC
                                                          : RADIOLIB_SX126X_STATUS_MODE_STDBY_RC;
        break;
    case RADIOLIB_SX126X_CMD_SET_RX:
        if (!chip.stuck_rx) { chip.mode = RADIOLIB_SX126X_STATUS_MODE_RX; }
        break;
    case RADIOLIB_SX126X_CMD_SET_RX_TX_FALLBACK_MODE: chip.fallback = out[1]; break;
    default: break;
    }
}

/* ---- fake HAL (radio_hal.h) ---------------------------------------------- */
static uint32_t nrst_level = 1;

NrfHal::NrfHal(uint32_t sck, uint32_t miso, uint32_t mosi, uint32_t busy)
    : RadioLibHal(NRFHAL_INPUT, NRFHAL_OUTPUT, NRFHAL_LOW, NRFHAL_HIGH, NRFHAL_RISING, NRFHAL_FALLING),
      _sck(sck), _miso(miso), _mosi(mosi), _busy(busy), _spiTimeout(false), _hang() {}
void NrfHal::init(void) {}
void NrfHal::term(void) {}
void NrfHal::pinMode(uint32_t, uint32_t) {}
void NrfHal::digitalWrite(uint32_t pin, uint32_t value)
{
    if (expired()) { _deadlineHit = true; return; }
    if (pin == PIN_LORA_NRST) {
        if (nrst_level == 0u && value != 0u) { chip_reset(); } /* released from reset */
        nrst_level = value;
    }
}
uint32_t NrfHal::digitalRead(uint32_t pin) { if (expired()) { _deadlineHit = true; return 0; } return pin == RADIOLIB_NC ? 0u : gpio_read(pin); }
void NrfHal::attachInterrupt(uint32_t, void (*)(void), uint32_t) {}
void NrfHal::detachInterrupt(uint32_t) {}
void NrfHal::delay(RadioLibTime_t ms) { now_us += (uint64_t)ms * 1000u; }
void NrfHal::delayMicroseconds(RadioLibTime_t us) { now_us += us; }
RadioLibTime_t NrfHal::millis(void) { return (RadioLibTime_t)(now_us / 1000u); }
RadioLibTime_t NrfHal::micros(void) { return (RadioLibTime_t)now_us; }
long NrfHal::pulseIn(uint32_t, uint32_t, RadioLibTime_t) { return 0; }
void NrfHal::yield(void) { now_us += 10u; }
void NrfHal::spiBegin(void) {}
void NrfHal::spiBeginTransaction(void) {}
void NrfHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    if (expired()) { _deadlineHit = _spiTimeout = true; memset(in, 0xff, len); return; }
    chip_transfer(out, in, len);
    now_us += len * 8u; /* 1 Mbps */
}
void NrfHal::spiEndTransaction(void) {}
void NrfHal::spiEnd(void) {}
static bool spim_timed_out;
bool NrfHal::takeSpiTimeout(void) { bool value = spim_timed_out; spim_timed_out = false; return value; }
void NrfHal::restartHangCheck(void) {}
void NrfHal::limit(uint32_t end_us) { _deadline = end_us; _limited = true; _deadlineHit = false; }
void NrfHal::unlimit(void) { _limited = _deadlineHit = false; }
bool NrfHal::expired() const { return _limited && (int32_t)(board_micros() - _deadline) >= 0; }

/* ---- scenarios ------------------------------------------------------------ */
/* One transmission through the MAC's start/poll calls: 0 sent, -1 failed. */
static int transmit(const uint8_t *data, int len)
{
    uint32_t until = board_micros() + TX_SETUP_MS * 1000u + LORA_TOA_US((unsigned)len) + 5000u;
    if (radio_tx_start(data, len, until) != 0) { return -1; }
    int done;
    do { done = radio_tx_poll(); } while (done == 0);
    return done == 1 ? 0 : -1;
}

static void power_on(void)
{
    chip_reset();
    set_line(PIN_LORA_BUSY, chip.busy);
    set_line(PIN_LORA_DIO1, false);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    if (!strcmp(scenario, "tx_missing_irq")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        uint8_t packet[94] = {0}; uint32_t end = board_micros() + 110000;
        assert(radio_tx_start(packet, sizeof(packet), end) == 0);
        assert(radio_tx_poll() == 0);
        now_us = end; auto commands = chip.frames.size();
        assert(radio_tx_poll() < 0 && radio_needs_reset());
        assert(chip.frames.size() == commands); /* reset uses GPIO, no stuck SPI */
        assert(test_gpio[PIN_LORA_NRST / 32].OUTCLR & (1u << (PIN_LORA_NRST % 32)));
        assert(radio_tx_start(packet, sizeof(packet), end + 200000) < 0);
        assert(radio_begin() == 0 && radio_start_rx() == 0 && !radio_needs_reset());
    } else if (!strcmp(scenario, "tx_busy_deadline")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        chip.busy = true; set_line(PIN_LORA_BUSY, true);
        uint8_t packet[94] = {0}; uint32_t start = board_micros();
        assert(radio_tx_start(packet, sizeof(packet), start + 110000) < 0);
        assert((uint32_t)(board_micros() - start) < 7200 && radio_needs_reset());
    } else if (!strcmp(scenario, "rx_access_deadline")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        chip.irq_flags = RADIOLIB_SX126X_IRQ_HEADER_VALID;
        uint32_t start = board_micros(); radio_limit(start + 30000);
        assert(radio_rx_settle(12, 10000) == RADIO_RX_BUSY);
        assert(board_micros() - start < 31200 && chip.mode == RADIOLIB_SX126X_STATUS_MODE_RX);
        radio_unlimit(); assert(!radio_needs_reset());
    } else if (!strcmp(scenario, "lbt_access_deadline") || !strcmp(scenario, "rx_access_boundary")) {
        const bool lbt = !strcmp(scenario, "lbt_access_deadline");
        const int cancelled = lbt ? RADIO_LBT_LOUD : RADIO_RX_BUSY;
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        for (unsigned wrap = 0; wrap < 2; wrap++) {
            for (unsigned budget : {0u, 1u, 16u, 40u, 100u, 1000u, 4000u, 4999u, 5000u, 5001u, 5010u, 5100u, 10000u}) {
                for (unsigned repeat = 0; repeat < RADIO_NORESP_RESET + 1u; repeat++) {
                    if (wrap) { now_us = UINT32_MAX - 20u; }
                    uint32_t start = board_micros(); radio_limit(start + budget);
                    int result = lbt ? radio_lbt_listen() : radio_rx_settle(0, 0);
                    uint32_t elapsed = (uint32_t)now_us - start;
                    assert(!radio_needs_reset() && !notes[EL_SPI_TIMEOUT]);
                    assert(chip.mode == RADIOLIB_SX126X_STATUS_MODE_RX);
                    assert(result == cancelled || (result == 0 && elapsed < budget));
                    if (lbt && budget <= LBT_LISTEN_MS * 1000u) { assert(result == cancelled); }
                    if (budget == 10000u) { assert(result == 0); }
                    assert(elapsed < budget + 200u); /* finish the healthy SPI command */
                    uint32_t restored;
                    assert(hal.getLimit(restored) && restored == start + budget);
                    radio_unlimit();
                }
            }
        }
    } else if (!strcmp(scenario, "lbt_busy_deadline") || !strcmp(scenario, "rx_busy_deadline")) {
        const bool lbt = !strcmp(scenario, "lbt_busy_deadline");
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        set_line(PIN_LORA_BUSY, true);
        uint32_t start = board_micros(); radio_limit(start + 100u);
        int result = lbt ? radio_lbt_listen() : radio_rx_settle(0, 0);
        assert(result == (lbt ? RADIO_LBT_LOUD : RADIO_RX_BUSY));
        assert(board_micros() - start < 7200u && radio_needs_reset());
        assert(notes[EL_SPI_TIMEOUT]);
        radio_unlimit();
    } else if (!strcmp(scenario, "cad_access_deadline")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        cad_delay_us = 2500; /* healthy scan, including expiry during SPI setup/result */
        for (unsigned wrap = 0; wrap < 2; wrap++) {
            if (wrap) { now_us = UINT32_MAX - 1000u; }
            for (unsigned budget : {0u, 1u, 32u, 200u, 2000u, 2800u, 10000u}) {
                for (unsigned repeat = 0; repeat < RADIO_NORESP_RESET + 1u; repeat++) {
                    uint32_t start = board_micros(); radio_limit(start + budget);
                    int result = radio_cad();
                    assert(result == RADIO_CAD_BUSY || result == RADIO_CAD_CLEAR);
                    if (budget < cad_delay_us) { assert(result == RADIO_CAD_BUSY); }
                    if (budget == 10000u) { assert(result == RADIO_CAD_CLEAR); }
                    assert(!radio_needs_reset() && !notes[EL_SPI_TIMEOUT] && !notes[EL_CAD_TIMEOUT]);
                    uint32_t restored;
                    assert(hal.getLimit(restored) && restored == start + budget);
                    if (result == RADIO_CAD_BUSY) {
                        assert(chip.mode == RADIOLIB_SX126X_STATUS_MODE_RX);
                        assert(board_micros() - start < budget + cad_delay_us + 1000u); /* awaited to completion */
                    }
                    radio_unlimit();
                    assert(radio_start_rx() == 0);
                }
            }
        }
    } else if (!strcmp(scenario, "cad_hung_deadline")) {
        /* CAD-done never comes. However late the slot was entered (here the deadline falls
         * 9, 7, 5, 2 and 0.1 ms into the scan), the scan counts as failed and resets the radio. */
        power_on();
        unsigned counted = 0;
        for (unsigned budget : {9000u, 7000u, 5000u, 2000u, 100u}) {
            assert(radio_begin() == 0 && radio_start_rx() == 0);
            for (unsigned i = 1; i <= RADIO_NORESP_RESET; i++) {
                assert(!radio_needs_reset());
                uint32_t start = board_micros(); radio_limit(start + budget);
                assert(radio_cad() == RADIO_CAD_ERROR && notes[EL_CAD_TIMEOUT] == ++counted);
                assert(board_micros() - start < (CAD_TIMEOUT_MS + 1u) * 1000u); /* bounded wait */
                radio_unlimit();
            }
            assert(radio_needs_reset());
        }
    } else if (!strcmp(scenario, "cad_busy_deadline")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        set_line(PIN_LORA_BUSY, true);
        uint32_t start = board_micros(); radio_limit(start + 100u);
        assert(radio_cad() == RADIO_CAD_ERROR);
        assert(board_micros() - start < 7200u && radio_needs_reset());
        assert(notes[EL_SPI_TIMEOUT] && notes[EL_CAD_TIMEOUT]);
        radio_unlimit();
    } else if (!strcmp(scenario, "probe_frame")) {
        /* first boot: RadioLib's Module still has its default framing */
        power_on();
        assert(radio_answers());
        const frame_t &f = chip.frames.front();
        assert(f.size() == 20 && f[0] == 0x1D && f[1] == 0x03 && f[2] == 0x20 && f[3] == 0x00);
    } else if (!strcmp(scenario, "probe_silent")) {
        chip.answers = false;
        power_on();
        uint64_t t0 = now_us;
        assert(!radio_answers());
        assert(now_us - t0 <= (RADIO_PROBE_MS + RADIO_SPI_TIMEOUT_MS + 5u) * 1000u);
    } else if (!strcmp(scenario, "probe_busy_stuck")) {
        chip.busy = true;
        power_on();
        uint64_t t0 = now_us;
        assert(!radio_answers());
        assert(now_us - t0 <= (RADIO_PROBE_MS + RADIO_SPI_TIMEOUT_MS + 5u) * 1000u);
    } else if (!strcmp(scenario, "begin")) {
        /* the whole bring-up: probe, begin(), then standby on the crystal oscillator */
        power_on();
        assert(radio_begin() == 0);
        assert(radio.tcxoVoltage == 0.0f);
        for (const frame_t &f : chip.frames) {
            assert(f[0] != RADIOLIB_SX126X_CMD_SET_DIO3_AS_TCXO_CTRL);
        }
        assert(chip.packet_type == RADIOLIB_SX126X_PACKET_TYPE_LORA);
        assert(chip.fallback == RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_XOSC);
        assert(chip.mode == RADIOLIB_SX126X_STATUS_MODE_STDBY_XOSC);
        assert(radio_needs_reset());
        assert(radio_start_rx() == 0);
        assert(!radio_needs_reset());
    } else if (!strcmp(scenario, "xosc_stuck")) {
        power_on(); chip.stuck_xosc = true;
        assert(radio_begin() != 0 && radio_needs_reset());
        assert(notes[EL_RADIO_INIT] > 0 && (active_faults & (1u << EL_RADIO_INIT)));
        chip.stuck_xosc = false;
        assert(radio_begin() == 0);
        assert(active_faults & (1u << EL_RADIO_INIT)); /* configured alone is not ready */
        assert(radio_start_rx() == 0 && !radio_needs_reset());
        assert(!(active_faults & (1u << EL_RADIO_INIT)));
    } else if (!strcmp(scenario, "rx_stuck")) {
        power_on(); assert(radio_begin() == 0);
        chip.stuck_rx = true;
        assert(radio_start_rx() != 0 && radio_needs_reset());
        assert(notes[EL_RADIO_INIT] == 1 && notes[EL_RX_FAIL] == 1);
        chip.stuck_rx = false;
        assert(radio_begin() == 0 && radio_start_rx() == 0 && !radio_needs_reset());
    } else if (!strcmp(scenario, "device_errors")) {
        power_on();
        for (uint16_t error : { RADIOLIB_SX126X_XOSC_START_ERR, RADIOLIB_SX126X_PLL_LOCK_ERR,
                                RADIOLIB_SX126X_IMG_CALIB_ERR }) {
            chip.device_errors = error;
            assert(radio_begin() != 0 && radio_needs_reset());
        }
        assert(notes[EL_RADIO_CLOCK] > 0 && notes[EL_RADIO_INIT] > 0);
        chip.device_errors = 0;
        assert(radio_begin() == 0);
        chip.device_errors = RADIOLIB_SX126X_PLL_LOCK_ERR;
        assert(radio_start_rx() != 0 && radio_needs_reset());
        chip.device_errors = 0;
        assert(radio_begin() == 0);
        assert(active_faults & (1u << EL_RADIO_CLOCK));
        assert(radio_start_rx() == 0 && !radio_needs_reset());
        assert(!(active_faults & ((1u << EL_RADIO_CLOCK) | (1u << EL_RADIO_INIT) | (1u << EL_RX_FAIL))));
    } else if (!strcmp(scenario, "errors_unreadable")) {
        power_on(); assert(radio_begin() == 0);
        chip.reject_errors = true;
        assert(radio_start_rx() != 0 && radio_needs_reset());
        assert(radio_begin() != 0 && radio_needs_reset());
        chip.reject_errors = false;
        assert(radio_begin() == 0 && radio_start_rx() == 0 && !radio_needs_reset());
    } else if (!strcmp(scenario, "init_recovery")) {
        power_on();
        assert(radio_begin() == 0 && radio_start_rx() == 0);
        chip.answers = false;
        assert(radio_begin() != 0 && radio_needs_reset());
        chip.answers = true;
        assert(radio_standby() == 0 && radio_needs_reset());
        assert(radio_start_rx() == 0 && radio_needs_reset()); /* begin never completed */
        assert(radio_begin() == 0 && radio_needs_reset());
        chip.reject_rx = true;
        assert(radio_start_rx() != 0 && radio_needs_reset());
        assert(radio_standby() == 0 && radio_needs_reset());
        assert(s_noresp == 0); /* sticky initialization failure survives answered commands */
        assert(radio_begin() == 0);
        assert(radio_start_rx() != 0 && radio_needs_reset()); /* failed recovery stays pending */
        chip.reject_rx = false;
        assert(radio_begin() == 0 && radio_start_rx() == 0 && !radio_needs_reset());
        chip.reject_rx = true;
        assert(radio_start_rx() != 0 && !radio_needs_reset()); /* ordinary RX re-arm policy unchanged */
    } else if (!strcmp(scenario, "config_range")) {
        /* the band limits are accepted, and the frequency reaches the chip */
        const radio_cfg_t cfgs[] = { { RADIO_FREQ_MIN_KHZ, RADIO_POWER_MIN_DBM, 0 },
                                     { RADIO_FREQ_MAX_KHZ, RADIO_POWER_MAX_DBM, 1 }, RADIO_CFG_DEFAULT };
        for (const radio_cfg_t &c : cfgs) {
            radio_set_config(&c);
            power_on();
            chip.frames.clear();
            assert(radio_begin() == 0 && radio_lbt_enabled() == c.lbt);
            uint32_t frf = 0;
            for (const frame_t &f : chip.frames) {
                if (f[0] == RADIOLIB_SX126X_CMD_SET_RF_FREQUENCY) {
                    frf = (uint32_t)f[1] << 24 | (uint32_t)f[2] << 16 | (uint32_t)f[3] << 8 | f[4];
                }
            }
            double want = c.freq_khz * 1000.0 * (1u << RADIOLIB_SX126X_DIV_EXPONENT) / 32e6;
            assert(frf + 64u > want && frf < want + 64u); /* float MHz in RadioLib: ~60 Hz steps */
        }
    } else if (!strcmp(scenario, "lbt_listen")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        uint64_t t0 = now_us;
        chip.rssi_raw = (uint8_t)(-2 * LBT_THRESHOLD_DBM); /* exactly the threshold: quiet */
        assert(radio_lbt_listen() == RADIO_LBT_QUIET);
        assert(now_us - t0 >= LBT_LISTEN_MS * 1000u && now_us - t0 < (LBT_LISTEN_MS + 1u) * 1000u);
        chip.rssi_raw = (uint8_t)(-2 * LBT_THRESHOLD_DBM - 1); /* 0.5 dB louder */
        assert(radio_lbt_listen() == RADIO_LBT_LOUD);
        chip.rssi_raw = 220;
        chip.answers = false; /* a read that fails is never quiet */
        assert(radio_lbt_listen() == RADIO_LBT_LOUD);
        assert(notes[EL_SPI_TIMEOUT] > 0);
    } else if (!strcmp(scenario, "cad_timeout")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        for (unsigned i = 0; i < RADIO_NORESP_RESET; i++) { assert(radio_cad() == RADIO_CAD_ERROR); }
        assert(radio_needs_reset() && notes[EL_CAD_TIMEOUT] == RADIO_NORESP_RESET);
    } else if (!strcmp(scenario, "cad_wrong_modem")) {
        power_on(); assert(radio_begin() == 0);
        chip.packet_type = RADIOLIB_SX126X_PACKET_TYPE_GFSK;
        assert(radio_cad() == RADIO_CAD_ERROR);
    } else if (!strcmp(scenario, "begin_dead")) {
        chip.answers = false;
        power_on();
        uint64_t t0 = now_us;
        assert(radio_begin() != 0);
        assert(radio_needs_reset());
        assert(now_us - t0 <= 5u * (RADIO_PROBE_MS + RADIO_SPI_TIMEOUT_MS + 5u) * 1000u); /* ~0.5 s, not ~10 s */
    } else if (!strcmp(scenario, "receive")) {
        power_on();
        assert(radio_begin() == 0);
        for (int i = 0; i < 60; i++) { chip.buf[i] = (uint8_t)(i * 7); }
        chip.rx_lengths = { 60 };
        set_line(PIN_LORA_DIO1, true);
        uint8_t buf[WIRE_MAX];
        assert(radio_receive(buf, sizeof(buf), NULL, NULL) == 60);
        for (int i = 0; i < 60; i++) { assert(buf[i] == (uint8_t)(i * 7)); }
    } else if (!strcmp(scenario, "fault_recovery")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        spim_timed_out = true;
        assert(track(RADIOLIB_ERR_NONE) == RADIOLIB_ERR_SPI_CMD_TIMEOUT);
        assert(active_faults & (1u << EL_SPI_TIMEOUT));
        assert(radio_standby() == 0);
        assert(!(active_faults & (1u << EL_SPI_TIMEOUT)) && notes[EL_SPI_TIMEOUT] == 1);
        assert(radio_cad() == RADIO_CAD_ERROR);
        assert(active_faults & (1u << EL_CAD_TIMEOUT));
        assert(radio_start_rx() == 0);
        assert(active_faults & (1u << EL_CAD_TIMEOUT)); /* unrelated success */
        chip.irq_flags = RADIOLIB_SX126X_IRQ_CAD_DONE;
        set_line(PIN_LORA_DIO1, true);
        assert(radio_cad() == RADIO_CAD_CLEAR);
        assert(!(active_faults & (1u << EL_CAD_TIMEOUT)) && notes[EL_CAD_TIMEOUT] == 1);
        uint8_t tx[8] = {0};
        set_line(PIN_LORA_DIO1, false);
        assert(transmit(tx, sizeof tx) != 0);
        assert(active_faults & (1u << EL_TX_FAIL));
        assert(radio_needs_reset());
        assert(radio_begin() == 0 && radio_start_rx() == 0);
        assert(active_faults & (1u << EL_TX_FAIL)); /* RX is not proof of TX */
        set_line(PIN_LORA_DIO1, true);
        assert(transmit(tx, sizeof tx) == 0);
        assert(!(active_faults & (1u << EL_TX_FAIL)) && notes[EL_TX_FAIL] == 1);
    } else if (!strcmp(scenario, "rx_fault_recovery")) {
        power_on(); assert(radio_begin() == 0 && radio_start_rx() == 0);
        uint8_t buf[WIRE_MAX];
        set_line(PIN_LORA_DIO1, true);
        chip.rx_lengths = { 0 };
        assert(radio_receive(buf, sizeof buf, NULL, NULL) < 0);
        assert(active_faults & (1u << EL_RX_FAIL));
        assert(radio_start_rx() == 0);
        assert(active_faults & (1u << EL_RX_FAIL)); /* re-arm is not a good packet */
        chip.rx_lengths = { 12 };
        chip.reject_rx = true;
        assert(radio_receive(buf, sizeof buf, NULL, NULL) == 12);
        assert(active_faults & (1u << EL_RX_FAIL)); /* good packet is not a good re-arm */
        chip.reject_rx = false;
        assert(radio_start_rx() == 0);
        assert(!(active_faults & (1u << EL_RX_FAIL)));
        assert(notes[EL_RX_FAIL] == 2); /* both past failures remain counted */
    } else if (!strcmp(scenario, "receive_zero_length")) {
        /* the length read fails (0) while the packet is long: never read past maxlen */
        power_on();
        assert(radio_begin() == 0);
        chip.rx_lengths = { 0, 200 };
        chip.frames.clear();
        set_line(PIN_LORA_DIO1, true);
        uint8_t buf[256];
        assert(radio_receive(buf, WIRE_MAX, NULL, NULL) < 0);
        for (const frame_t &f : chip.frames) {
            if (f[0] == RADIOLIB_SX126X_CMD_READ_BUFFER) { assert(f.size() <= 3u + WIRE_MAX); }
        }
        assert(notes[EL_RX_FAIL] == 1);
    } else {
        fprintf(stderr, "unknown scenario: %s\n", scenario);
        return 2;
    }
    printf("PASS %s\n", scenario);
    return 0;
}
