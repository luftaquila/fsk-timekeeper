#include "radio_hal.h"

#include "nrf.h"
#include "gpio.h"
#include "board.h" /* board_micros / board_millis — single timebase owner */

static inline uint32_t now_us(void)
{
    return board_micros();
}

NrfHal::NrfHal(uint32_t sck, uint32_t miso, uint32_t mosi)
    : RadioLibHal(NRFHAL_INPUT, NRFHAL_OUTPUT, NRFHAL_LOW, NRFHAL_HIGH,
                  NRFHAL_RISING, NRFHAL_FALLING),
      _sck(sck), _miso(miso), _mosi(mosi)
{
}

void NrfHal::init(void)
{
    /* Timebase (TIMER2) is started by board_init(); just bring up SPI here. */
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

/* Blocking TX/RX poll DIO1; no ISR needed yet (Stage 3 brings GPIOTE capture). */
void NrfHal::attachInterrupt(uint32_t, void (*)(void), uint32_t) {}
void NrfHal::detachInterrupt(uint32_t) {}

void NrfHal::delay(RadioLibTime_t ms)
{
    uint32_t start = now_us();
    uint32_t target = ms * 1000UL;
    while ((uint32_t)(now_us() - start) < target) {
    }
}

void NrfHal::delayMicroseconds(RadioLibTime_t us)
{
    uint32_t start = now_us();
    while ((uint32_t)(now_us() - start) < (uint32_t)us) {
    }
}

RadioLibTime_t NrfHal::millis(void)
{
    return now_us() / 1000UL;
}

RadioLibTime_t NrfHal::micros(void)
{
    return now_us();
}

long NrfHal::pulseIn(uint32_t, uint32_t, RadioLibTime_t)
{
    return 0; /* unused by SX126x */
}

static inline uint32_t psel(uint32_t pin)
{
    /* pin == port*32 + n already encodes PORT(bit5)|PIN(bits0-4); CONNECT bit clear. */
    return pin;
}

void NrfHal::spiBegin(void)
{
    /* SCK/MOSI as outputs (SCK idle low for SPI mode 0), MISO as input. */
    gpio_clear(_sck);
    gpio_cfg_output(_sck);
    gpio_clear(_mosi);
    gpio_cfg_output(_mosi);
    gpio_cfg_input(_miso);

    NRF_SPIM0->PSEL.SCK = psel(_sck);
    NRF_SPIM0->PSEL.MOSI = psel(_mosi);
    NRF_SPIM0->PSEL.MISO = psel(_miso);
    /* 1 Mbps — conservative for the hand-built board's unterminated SPI traces.
     * 8 MHz gave corrupted readback (WRONG_MODEM / SPI_CMD_FAILED); SX1262
     * config traffic is tiny so the low rate costs nothing. */
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M1;
    NRF_SPIM0->CONFIG = 0; /* mode 0, MSB first */
    NRF_SPIM0->ENABLE = (SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos);
}

void NrfHal::spiBeginTransaction(void) {}

void NrfHal::spiTransfer(uint8_t* out, size_t len, uint8_t* in)
{
    if (len == 0) {
        return;
    }
    NRF_SPIM0->TXD.PTR = (uint32_t)out;
    NRF_SPIM0->TXD.MAXCNT = len;
    NRF_SPIM0->RXD.PTR = (uint32_t)in;
    NRF_SPIM0->RXD.MAXCNT = len;
    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->TASKS_START = 1;
    while (NRF_SPIM0->EVENTS_END == 0) {
    }
    NRF_SPIM0->EVENTS_END = 0;
}

void NrfHal::spiEndTransaction(void) {}

void NrfHal::spiEnd(void)
{
    NRF_SPIM0->ENABLE = (SPIM_ENABLE_ENABLE_Disabled << SPIM_ENABLE_ENABLE_Pos);
}
