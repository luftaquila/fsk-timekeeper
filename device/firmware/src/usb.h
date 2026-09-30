/* USB CDC-ACM serial: the master's PC link, provisioning on every board, and
 * the 1200-baud bootloader touch. */
#ifndef USB_H
#define USB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up USB power events + TinyUSB. Call once after board_init() and node_init(). */
void usb_init(void);

/* Pump the TinyUSB device stack. Call every main-loop pass. */
void usb_task(void);

/* Queue one complete NUL-terminated line if the CDC FIFO has room for all of it.
 * Returns 1 only when every byte was queued. */
int usb_write(const char *s);

/* One byte from the CDC port (0-255), or -1 if none. */
int usb_read_byte(void);

/* Non-zero when a USB host has enumerated us (TinyUSB mounted): a PC data link,
 * not just a charger. Basis of the boot-time role decision (DESIGN §8). */
int usb_host_present(void);

/* Non-zero while VBUS is present (POWER.USBREGSTATUS.VBUSDETECT). Stays set
 * while a connected host merely suspends the bus. */
int usb_vbus_present(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_H */
