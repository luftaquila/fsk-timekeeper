/* TinyUSB configuration — nRF52840 device, single CDC-ACM (serial), no OS, no
 * SoftDevice (SOFTDEVICE_PRESENT intentionally undefined so the nRF dcd drives
 * HFCLK directly). */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_NRF5X
#define CFG_TUSB_OS            OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE  OPT_MODE_DEVICE

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#define CFG_TUD_ENABLED        1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC            1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

#define CFG_TUD_MSC            0
#define CFG_TUD_HID            0
#define CFG_TUD_MIDI           0
#define CFG_TUD_VENDOR         0

#endif /* TUSB_CONFIG_H */
