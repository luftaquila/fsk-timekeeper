#include "radio_hal.h"

#include "nrf.h"
#include "gpio.h"
#include "board.h"
#include "errlog.h"
#include "fault.h"

extern "C" {
#include "config.h"
}

/* A 1 MHz transfer of the largest SX1262 command (~260 B) takes ~2 ms. */
#define SPI_TRANSFER_MAX_US 10000u

NrfHal::NrfHal(uint32_t sck, uint32_t miso, uint32_t mosi, uint32_t busy)
    : RadioLibHal(NRFHAL_INPUT, NRFHAL_OUTPUT, NRFHAL_LOW, NRFHAL_HIGH,
                  NRFHAL_RISING, NRFHAL_FALLING),
      _sck(sck), _miso(miso), _mosi(mosi), _busy(busy), _spiTimeout(false), _hang()
{
}

void NrfHal::init(void)
{
    /* TIMER2 is started by board_init(); only SPI here. */
    spiBegin();
}

void NrfHal::term(void)
{
    spiEnd();
}

void NrfHal::pinMode(uint32_t pin, uint32_t mode)
{
    if (pin == RADIOLIB_NC) {
        return;
    }
    if (mode == NRFHAL_OUTPUT) {
        gpio_cfg_output(pin);
    } else {
        gpio_cfg_input(pin);
    }
}

void NrfHal::digitalWrite(uint32_t pin, uint32_t value)
{
    if (pin == RADIOLIB_NC) {
        return;
    }
    gpio_write(pin, value != 0);
}

uint32_t NrfHal::digitalRead(uint32_t pin)
{
    if (pin == RADIOLIB_NC) {
        return 0;
    }
    return gpio_read(pin);
}

void NrfHal::attachInterrupt(uint32_t, void (*)(void), uint32_t) {}
void NrfHal::detachInterrupt(uint32_t) {}

void NrfHal::delay(RadioLibTime_t ms)
{
    uint32_t start = board_micros();
    uint32_t target = ms * 1000UL;
    while ((uint32_t)(board_micros() - start) < target) {
    }
}

void NrfHal::delayMicroseconds(RadioLibTime_t us)
{
    uint32_t start = board_micros();
    while ((uint32_t)(board_micros() - start) < (uint32_t)us) {
    }
}

/* RadioLib compares these by unsigned subtraction: both wrap modulo 2^32
 * (millis every ~49 days, micros every ~71.6 min). */
RadioLibTime_t NrfHal::millis(void)
{
    return board_millis();
}

RadioLibTime_t NrfHal::micros(void)
{
    return board_micros();
}

long NrfHal::pulseIn(uint32_t, uint32_t, RadioLibTime_t)
{
    return 0; /* unused by SX126x */
}

/* Called on every turn of RadioLib's BUSY loops. Bounded waits end within
 * RADIO_SPI_TIMEOUT_MS; BUSY high for RADIO_HANG_MS means a loop without a
 * timeout will never end. PC = the waiting RadioLib code. */
void NrfHal::yield(void)
{
    if (hang_guard_poll(&_hang, gpio_read(_busy) != 0u, board_millis(), RADIO_HANG_MS)) {
        fault_hang((uint32_t)__builtin_return_address(0), FAULT_CAUSE_RADIO);
    }
}

void NrfHal::restartHangCheck(void)
{
    hang_guard_reset(&_hang);
}

void NrfHal::spiBegin(void)
{
    /* Also runs again on every radio reset: PSEL may only change while SPIM is
     * disabled. SCK/MOSI outputs (SCK idle low, SPI mode 0), MISO input.
     * PSEL = port*32+pin. */
    NRF_SPIM0->ENABLE = (SPIM_ENABLE_ENABLE_Disabled << SPIM_ENABLE_ENABLE_Pos);
    gpio_clear(_sck);
    gpio_cfg_output(_sck);
    gpio_clear(_mosi);
    gpio_cfg_output(_mosi);
    gpio_cfg_input(_miso);

    NRF_SPIM0->PSEL.SCK = _sck;
    NRF_SPIM0->PSEL.MOSI = _mosi;
    NRF_SPIM0->PSEL.MISO = _miso;
    /* 1 Mbps: 8 MHz corrupted readback on the hand-built board's traces. */
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M1;
    NRF_SPIM0->CONFIG = 0; /* mode 0, MSB first */
    NRF_SPIM0->ENABLE = (SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos);
}

void NrfHal::spiBeginTransaction(void) {}

void NrfHal::spiTransfer(uint8_t* out, size_t len, uint8_t* in)
{
    hang_guard_reset(&_hang); /* the radio is taking commands */
    if (len == 0) {
        return;
    }
    NRF_SPIM0->TXD.PTR = (uint32_t)out;
    NRF_SPIM0->TXD.MAXCNT = len;
    NRF_SPIM0->RXD.PTR = (uint32_t)in;
    NRF_SPIM0->RXD.MAXCNT = len;
    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->TASKS_START = 1;
    uint32_t t0 = board_micros();
    while (NRF_SPIM0->EVENTS_END == 0) {
        if ((uint32_t)(board_micros() - t0) >= SPI_TRANSFER_MAX_US) {
            /* RadioLib sees garbage status and fails the command; the radio
             * layer counts this as a no-response. */
            NRF_SPIM0->TASKS_STOP = 1;
            el_note(EL_HW_TIMEOUT);
            _spiTimeout = true;
            return;
        }
    }
    NRF_SPIM0->EVENTS_END = 0;
}

void NrfHal::spiEndTransaction(void) {}

void NrfHal::spiEnd(void)
{
    NRF_SPIM0->ENABLE = (SPIM_ENABLE_ENABLE_Disabled << SPIM_ENABLE_ENABLE_Pos);
}

bool NrfHal::takeSpiTimeout(void)
{
    bool t = _spiTimeout;
    _spiTimeout = false;
    return t;
}
