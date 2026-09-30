/* radio.cpp with the vendor RadioLib against a fake SX1262 behind a fake HAL: the
 * bytes the probe and begin() send, and the chip state they leave. Every
 * scenario runs in a new process, as on MCU boot. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/radio.cpp"
#include "../src/protocol.h"

/* ---- clock --------------------------------------------------------------- */
static uint64_t now_us;

extern "C" uint32_t board_micros(void) { return (uint32_t)now_us; }
/* Every poll moves time on, so polling loops end as they do on hardware. */
extern "C" uint32_t board_millis(void) { now_us += 1u; return (uint32_t)(now_us / 1000u); }
extern "C" void board_delay_ms(uint32_t ms) { now_us += (uint64_t)ms * 1000u; }

static unsigned notes[EL_COUNT];
extern "C" void el_note(el_code_t code) { notes[code]++; }

/* ---- fake SX1262 ----------------------------------------------------------- */
typedef std::vector<uint8_t> frame_t;

static struct {
    bool answers = true;        /* drives MISO; a dead radio reads 0xFF */
    bool busy = false;          /* BUSY stuck high */
    uint8_t reg[0x10000];
    uint8_t buf[256];
    uint8_t packet_type = 0;    /* GFSK after reset */
    uint8_t mode = RADIOLIB_SX126X_STATUS_MODE_STDBY_RC;
    uint8_t fallback = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_RC;
    std::vector<uint8_t> rx_lengths; /* answers of GetRxBufferStatus, last one repeats */
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

/* Command status 0x2 (processed) in the current chip mode. */
static uint8_t chip_status(void) { return (uint8_t)(chip.mode | 0x02u); }

static void chip_transfer(const uint8_t *out, uint8_t *in, size_t len)
{
    chip.frames.emplace_back(out, out + len);
    if (!chip.answers) { memset(in, 0xFF, len); return; }
    memset(in, chip_status(), len);
    uint16_t addr = len >= 3 ? (uint16_t)(out[1] << 8 | out[2]) : 0;
    switch (out[0]) {
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
    case RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS:
    case RADIOLIB_SX126X_CMD_GET_IRQ_STATUS:
        for (size_t i = 2; i < len; i++) { in[i] = 0; }
        break;
    case RADIOLIB_SX126X_CMD_GET_RX_BUFFER_STATUS:
        if (len > 3) {
            in[2] = chip.rx_lengths.empty() ? 0 : chip.rx_lengths.front();
            in[3] = 0;
            if (chip.rx_lengths.size() > 1) { chip.rx_lengths.erase(chip.rx_lengths.begin()); }
        }
        break;
    case RADIOLIB_SX126X_CMD_SET_STANDBY:
        chip.mode = out[1] == RADIOLIB_SX126X_STANDBY_XOSC ? RADIOLIB_SX126X_STATUS_MODE_STDBY_XOSC
                                                          : RADIOLIB_SX126X_STATUS_MODE_STDBY_RC;
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
    if (pin == PIN_LORA_NRST) {
        if (nrst_level == 0u && value != 0u) { chip_reset(); } /* released from reset */
        nrst_level = value;
    }
}
uint32_t NrfHal::digitalRead(uint32_t pin) { return pin == RADIOLIB_NC ? 0u : gpio_read(pin); }
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
    chip_transfer(out, in, len);
    now_us += len * 8u; /* 1 Mbps */
}
void NrfHal::spiEndTransaction(void) {}
void NrfHal::spiEnd(void) {}
bool NrfHal::takeSpiTimeout(void) { return false; }
void NrfHal::restartHangCheck(void) {}

/* ---- scenarios ------------------------------------------------------------ */
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
    if (!strcmp(scenario, "probe_frame")) {
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
        /* the whole bring-up: probe, begin(), then standby on the TCXO */
        power_on();
        assert(radio_begin() == 0);
        assert(chip.packet_type == RADIOLIB_SX126X_PACKET_TYPE_LORA);
        assert(chip.fallback == RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_XOSC);
        assert(chip.mode == RADIOLIB_SX126X_STATUS_MODE_STDBY_XOSC);
        assert(!radio_needs_reset());
    } else if (!strcmp(scenario, "begin_dead")) {
        chip.answers = false;
        power_on();
        uint64_t t0 = now_us;
        assert(radio_begin() != 0);
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
