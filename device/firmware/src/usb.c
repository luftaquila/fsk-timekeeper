#include "usb.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "tusb.h"
#include "nrfx_power.h"
#include "nrf.h"

/* TinyUSB 0.20.0's nRF dcd expects the nrfx v1/v2 HAL function
 * nrf_clock_hf_is_running(), which nrfx v4 renamed to nrf_clock_is_running().
 * Provide the old symbol as a thin register read so the dcd links unchanged. */
bool nrf_clock_hf_is_running(void *p_reg, uint32_t hfclk_src)
{
    NRF_CLOCK_Type *clk = (NRF_CLOCK_Type *)p_reg;
    uint32_t stat = clk->HFCLKSTAT;
    return (stat & CLOCK_HFCLKSTAT_STATE_Msk) &&
           (((stat & CLOCK_HFCLKSTAT_SRC_Msk) >> CLOCK_HFCLKSTAT_SRC_Pos) == hfclk_src);
}

/* TinyUSB nRF dcd entry point fed by the USB power events. */
extern void tusb_hal_nrf_power_event(uint32_t event);
extern void nrfx_power_irq_handler(void);

/* Vector handlers — neither nrfx_power nor the dcd installs these, so we route
 * the combined CLOCK/POWER IRQ to nrfx_power, and USBD to the TinyUSB dcd. */
void CLOCK_POWER_IRQHandler(void)
{
    nrfx_power_irq_handler();
}

void USBD_IRQHandler(void)
{
    dcd_int_handler(0);
}

static void power_usb_event_handler(nrfx_power_usb_evt_t event)
{
    tusb_hal_nrf_power_event((uint32_t)event);
}

void usb_init(void)
{
    const nrfx_power_config_t pwr_cfg = {0};
    (void)nrfx_power_init(&pwr_cfg);

    const nrfx_power_usbevt_config_t usb_cfg = {
        .handler = power_usb_event_handler,
    };
    nrfx_power_usbevt_init(&usb_cfg);
    nrfx_power_usbevt_enable();

    /* USB power is already present (we boot plugged in), so the DETECTED/READY
     * events fired before the IRQ was enabled — no interrupt will come. Seed the
     * dcd from the current regulator status, exactly as the TinyUSB nRF BSP does. */
    uint32_t usbreg = NRF_POWER->USBREGSTATUS;
    if (usbreg & POWER_USBREGSTATUS_VBUSDETECT_Msk) {
        tusb_hal_nrf_power_event((uint32_t)NRFX_POWER_USB_EVT_DETECTED);
    }
    if (usbreg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) {
        tusb_hal_nrf_power_event((uint32_t)NRFX_POWER_USB_EVT_READY);
    }

    tud_init(0);
}

void usb_task(void)
{
    tud_task();
}

int usb_write(const char *s)
{
    /* Queue a whole line or none of it. The reliable EVENT path retries the
     * line from the master queue; diagnostics may remain best-effort. Gating on
     * tud_cdc_connected() (DTR) would still be wrong because some hosts do not
     * assert DTR. */
    size_t len = strlen(s);
    if (tud_cdc_write_available() < len) { return 0; }
    uint32_t written = tud_cdc_write(s, len);
    tud_cdc_write_flush();
    return written == len;
}

int usb_connected(void)
{
    return tud_cdc_connected();
}

int usb_host_present(void)
{
    /* tud_mounted() is set once a host completes enumeration (SET_CONFIGURATION)
     * and cleared on cable removal / bus reset. Unlike VBUS detection it stays
     * false for a charger with no USB host, and unlike tud_cdc_connected() (DTR)
     * it does not depend on the host app opening the port. */
    return tud_mounted() ? 1 : 0;
}

int usb_read_byte(void)
{
    if (tud_cdc_available()) {
        uint8_t c;
        if (tud_cdc_read(&c, 1) == 1) {
            return c;
        }
    }
    return -1;
}

/* ---- 1200-baud touch -> enter the Adafruit UF2 bootloader (DFU) ----
 * The host opening the port at 1200 baud and dropping DTR is the standard
 * "reset to bootloader" gesture (adafruit-nrfutil --touch 1200). We stash the
 * baud and, on DTR deassert at 1200, set the bootloader magic and reset. */
static uint32_t s_last_baud;

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    s_last_baud = coding->bit_rate;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (!dtr && s_last_baud == 1200) {
        NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET (Adafruit nRF52 bootloader) */
        NVIC_SystemReset();
    }
}
