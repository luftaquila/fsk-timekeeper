#include "board.h"

#include "config.h"
#include "errlog.h"
#include "gpio.h"
#include "nrf.h"

/* App is linked at the S140 v6 user-app base (0x26000); point the core at our
 * vector table so interrupts dispatch to us, not the SoftDevice's. */
#define APP_VECTOR_BASE 0x00026000UL

/* TIMER1 (the capture timebase) inherits the HFCLK source's accuracy: HFINT is
 * only ~1 %, so every role runs on the crystal. */
static int hfclk_is_xtal(void)
{
    return (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk) &&
           (((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk) >> CLOCK_HFCLKSTAT_SRC_Pos)
                == CLOCK_HFCLKSTAT_SRC_Xtal);
}

static void hfclk_init(void)
{
    /* The bootloader may hand off with HFXO already running; HFCLKSTART then
     * raises no new HFCLKSTARTED event, so only start it when needed, and bound
     * the wait so a dead crystal cannot hang USB provisioning. */
    if (hfclk_is_xtal()) {
        return;
    }
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    for (volatile uint32_t i = 0; i < 1000000u && NRF_CLOCK->EVENTS_HFCLKSTARTED == 0; i++) {
    }
}

int board_hfclk_xtal(void)
{
    return hfclk_is_xtal();
}

static uint32_t s_hfxo_stops;

uint32_t board_hfxo_stops(void)
{
    return s_hfxo_stops;
}

void board_hfxo_service(void)
{
    static int restarting;
    static uint32_t requested_ms;
    if (hfclk_is_xtal()) {
        restarting = 0;
        return;
    }
    uint32_t now = board_millis();
    if (!restarting) {
        el_note(EL_HFXO_RESTART);
        s_hfxo_stops++;
        restarting = 1;
    } else if ((uint32_t)(now - requested_ms) < 1000u) {
        return;
    }
    requested_ms = now;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
}

static void timebase_init(void)
{
    /* TIMER2 at 1 MHz (16 MHz / 2^4). TIMER1 is the capture timebase. */
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
    /* Extend before dividing so millis wraps at ~49 days, not with micros. */
    static uint32_t previous;
    static uint64_t elapsed;
    uint32_t now = board_micros();
    elapsed += (uint32_t)(now - previous);
    previous = now;
    return (uint32_t)(elapsed / 1000u);
}

/* The CPU stalls during NVMC operations (a page erase ~85 ms); the bound only
 * catches a controller that never reports ready. */
static int nvmc_wait(void)
{
    for (uint32_t i = 0; i < 20000000u; i++) {
        if (NRF_NVMC->READY != NVMC_READY_READY_Busy) { return 1; }
    }
    el_note(EL_HW_TIMEOUT);
    return 0;
}

/* P0.09/P0.10 (GPS PPS/TXD) are NFC pins until UICR.NFCPINS.PROTECT is cleared.
 * Clear it once on a board's first boot, verify, and reset so it takes effect;
 * a failed write boots without GPS instead of looping. App DFU never erases UICR.
 * Must precede every other pin setup. */
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

void board_led_off(void)
{
    gpio_clear(PIN_LED_STATUS);
}

void board_led_toggle(void)
{
    gpio_toggle(PIN_LED_STATUS);
}

void board_delay_ms(uint32_t ms)
{
    uint32_t start = board_micros();
    uint32_t target = ms * 1000UL;
    while ((uint32_t)(board_micros() - start) < target) {
    }
}
