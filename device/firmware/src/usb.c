#include "usb.h"
#include "nrf_clock_compat.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "tusb.h"
#include "device/dcd.h"
#include "nrfx_power.h"
#include "nrf.h"

/* The REMOVED handling below mirrors this exact TinyUSB release's dcd_nrf5x.c. */
#if TUSB_VERSION_MAJOR != 0 || TUSB_VERSION_MINOR != 20 || TUSB_VERSION_REVISION != 0
#error "usb.c: re-verify power_usb_event_handler against the new TinyUSB dcd_nrf5x.c"
#endif

/* TinyUSB 0.20.0's dcd calls nrf_clock_hf_is_running(), an nrfx v1/v2 name renamed in v4. */
bool nrf_clock_hf_is_running(void *p_reg, uint32_t hfclk_src)
{
    NRF_CLOCK_Type *clk = (NRF_CLOCK_Type *)p_reg;
    uint32_t stat = clk->HFCLKSTAT;
    return (stat & CLOCK_HFCLKSTAT_STATE_Msk) &&
           (((stat & CLOCK_HFCLKSTAT_SRC_Msk) >> CLOCK_HFCLKSTAT_SRC_Pos) == hfclk_src);
}

extern void tusb_hal_nrf_power_event(uint32_t event);
extern void nrfx_power_irq_handler(void);

void CLOCK_POWER_IRQHandler(void)
{
    nrfx_power_irq_handler();
}

void USBD_IRQHandler(void)
{
    dcd_int_handler(0);
}

/* POWER IRQ. Handle REMOVED here: TinyUSB's path also stops HFCLK, dropping TIMER1 to RC. */
static void power_usb_event_handler(nrfx_power_usb_evt_t event)
{
    if (event != NRFX_POWER_USB_EVT_REMOVED) {
        tusb_hal_nrf_power_event((uint32_t)event);
        return;
    }
    if (NRF_USBD->ENABLE) {
        NRF_USBD->USBPULLUP = 0;
        __ISB();
        __DSB();
        NVIC_DisableIRQ(USBD_IRQn);
        NRF_USBD->INTENCLR = NRF_USBD->INTEN;
        NRF_USBD->ENABLE = 0;
        __ISB();
        __DSB();
        dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, true);
    }
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

    /* Booted plugged in, DETECTED/READY fired before the IRQ: replay them from USBREGSTATUS. */
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

static uint32_t s_console_epoch;
static bool s_console_dtr;

int usb_console_ready(void) { return tud_cdc_connected() ? 1 : 0; }
uint32_t usb_console_epoch(void) { return s_console_epoch; }

void tud_mount_cb(void)
{
    s_console_dtr = false;
    s_console_epoch++;
}

void tud_umount_cb(void)
{
    s_console_dtr = false;
    s_console_epoch++;
}

int usb_write(const char *s)
{
    /* Whole line or nothing; callers that need delivery retry. */
    if (!usb_console_ready()) { return 0; }
    size_t len = strlen(s);
    if (tud_cdc_write_available() < len) { return 0; }
    uint32_t written = tud_cdc_write(s, len);
    tud_cdc_write_flush();
    return written == len;
}

int usb_host_present(void)
{
    /* Configured by a host: false on a charger (unlike VBUS), no open port needed (unlike DTR). */
    return tud_mounted() ? 1 : 0;
}

int usb_vbus_present(void)
{
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) ? 1 : 0;
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

/* 1200-baud touch (open at 1200 baud, drop DTR) reboots into the Adafruit UF2 bootloader. */
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
    if (s_console_dtr != dtr) {
        s_console_dtr = dtr;
        s_console_epoch++;
    }
    if (!dtr) {
        tud_cdc_write_clear();
        tud_cdc_read_flush();
    }
    if (!dtr && s_last_baud == 1200) {
        NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET (Adafruit nRF52 bootloader) */
        NVIC_SystemReset();
    }
}
