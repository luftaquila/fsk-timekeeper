/* USB CDC-ACM: the master's PC link, provisioning on every board, 1200-baud bootloader touch. */
#ifndef USB_H
#define USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up USB power events + TinyUSB. Call once after board_init() and node_init(). */
void usb_init(void);

/* Pump the TinyUSB device stack. Call every main-loop pass. */
void usb_task(void);

/* Queue a whole NUL-terminated line or nothing; returns 1 only when every byte was queued. */
int usb_write(const char *s);

/* Ready = a console opened the port with DTR; enumeration alone does not permit reports.
 * Epoch changes void old command fragments and pending replies, even a close/reopen in a pass. */
int usb_console_ready(void);
uint32_t usb_console_epoch(void);

/* One byte from the CDC port (0-255), or -1 if none. */
int usb_read_byte(void);

/* Non-zero while a USB host has us enumerated (a PC link, not a charger); decides the boot role. */
int usb_host_present(void);

/* Non-zero while VBUS is present, also while a connected host merely suspends the bus. */
int usb_vbus_present(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_H */
