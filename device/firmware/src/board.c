#include "board.h"

#include "config.h"
#include "gpio.h"
#include "nrf.h"

/* App is linked at the S140 v6 user-app base (0x26000); point the core at our
 * vector table so interrupts dispatch to us, not the SoftDevice's. */
#define APP_VECTOR_BASE 0x00026000UL

/* Switch HFCLK from the 64 MHz internal RC (HFINT, the reset default) to the
 * external crystal (HFXO). The TIMERs derive PCLK16M from HFCLK, so the capture
 * timebase (TIMER1) inherits the source's accuracy. HFINT is only ~±1-2% — two
 * nodes on RC drift apart by ~1% (skew_ppm reads ~10000 and never settles). The
 * external SX1262 radio does not need the nRF RADIO peripheral, so nothing else
 * forces HFXO on; previously only the USB-connected master got HFXO for free
 * (TinyUSB starts it on VBUS), leaving battery sensors on RC. Start it here so
 * every role's timebase is crystal-disciplined (±40 ppm) regardless of USB. */
static int hfclk_is_xtal(void)
{
    return (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk) &&
           (((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk) >> CLOCK_HFCLKSTAT_SRC_Pos)
                == CLOCK_HFCLKSTAT_SRC_Xtal);
}

static void hfclk_init(void)
{
    /* The nice!nano/Adafruit bootloader uses USB, so it hands off with HFXO
     * ALREADY running. Re-triggering HFCLKSTART in that state does not regenerate
     * EVENTS_HFCLKSTARTED, so an unconditional wait spins forever and the boot
     * hangs before USB ever comes up. Guard: if HFCLK is already sourced from the
     * crystal, there's nothing to do; otherwise start it with a bounded wait that
     * can never hang (HFXO normally settles in <1 ms). */
    if (hfclk_is_xtal()) {
        return;
    }
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    for (volatile uint32_t i = 0; i < 1000000u && NRF_CLOCK->EVENTS_HFCLKSTARTED == 0; i++) {
        /* bounded so a failed crystal cannot hang provisioning over USB */
    }
}

int board_hfclk_xtal(void)
{
    return hfclk_is_xtal();
}

static void timebase_init(void)
{
    /* TIMER2 free-running at 1 MHz (16 MHz / 2^4). TIMER0 belongs to the
     * (unused) SoftDevice, TIMER1 is reserved for the Stage-3 capture base. */
    NRF_TIMER2->TASKS_STOP = 1;
    NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER2->PRESCALER = 4;
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_TIMER2->TASKS_START = 1;
}

uint32_t board_micros(void)
{
    NRF_TIMER2->TASKS_CAPTURE[0] = 1;
    return NRF_TIMER2->CC[0];
}

uint32_t board_millis(void)
{
    /* Extend before dividing: micros wraps at ~71 min, whereas callers use
     * uint32 subtraction expecting millis to wrap at ~49 days. The main loop
     * calls this much more often than once per micros wrap. */
    static uint32_t previous;
    static uint64_t elapsed;
    uint32_t now = board_micros();
    elapsed += (uint32_t)(now - previous);
    previous = now;
    return (uint32_t)(elapsed / 1000u);
}

/* P0.09/P0.10 are the NFC antenna pins until UICR.NFCPINS.PROTECT is cleared
 * (factory default = NFC, with a clamp diode between the two pins). The GPS PPS
 * and TXD lines sit on them, so clear the bit once, on a board's first boot:
 * NVMC write-enable, clear the bit (flash writes only clear bits, and the factory
 * word is erased so one write is within spec), read back, then reset so the new
 * UICR takes effect. Readback-guarded — a failed write cannot loop the board
 * through resets, it just boots without GPS. App DFU never erases UICR, so this
 * runs once per board. Runs in both roles (harmless on a sensor). Must precede
 * every other peripheral/pin setup. */
static void nvmc_wait(void)
{
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) { /* CPU stalls during op */ }
}

static void nfc_pins_as_gpio(void)
{
    if ((NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) == 0) {
        return;
    }
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos);
    nvmc_wait();
    NRF_UICR->NFCPINS = NRF_UICR->NFCPINS & ~UICR_NFCPINS_PROTECT_Msk;
    nvmc_wait();
    NRF_NVMC->CONFIG = (NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos);
    nvmc_wait();
    if ((NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) == 0) {
        NVIC_SystemReset();
    }
}

int board_nfc_pins_gpio(void)
{
    return (NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) == 0;
}

void board_init(void)
{
    nfc_pins_as_gpio();

    SCB->VTOR = APP_VECTOR_BASE;

    hfclk_init();
    timebase_init();
    board_ext_power_on();

    gpio_cfg_output(PIN_LED_STATUS);
    board_led_off();
}

void board_ext_power_on(void)
{
    gpio_cfg_output(PIN_EXT_POWER);
    gpio_set(PIN_EXT_POWER);
}

void board_ext_power_off(void)
{
    gpio_clear(PIN_EXT_POWER);
}

void board_led_on(void)
{
    gpio_set(PIN_LED_STATUS);
}

void board_led_off(void)
{
    gpio_clear(PIN_LED_STATUS);
}

void board_led_toggle(void)
{
    gpio_toggle(PIN_LED_STATUS);
}

void board_led_write(int on)
{
    gpio_write(PIN_LED_STATUS, on);
}

void board_delay_ms(uint32_t ms)
{
    uint32_t start = board_micros();
    uint32_t target = ms * 1000UL;
    while ((uint32_t)(board_micros() - start) < target) {
    }
}
