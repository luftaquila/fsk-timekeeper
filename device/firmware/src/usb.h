/* USB CDC-ACM serial: PC output for the master role + host-triggered bootloader
 * entry (1200-baud touch). */
#ifndef USB_H
#define USB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up nRF USB power events + TinyUSB. Call once after board_init(). */
void usb_init(void);

/* Pump the TinyUSB device stack. Call frequently from the main loop. */
void usb_task(void);

/* Queue one complete NUL-terminated line when the CDC FIFO has room. Returns 1
 * only when every byte was queued. Callers that need delivery retain and retry
 * until an application-level acknowledgement arrives. */
int usb_write(const char *s);

/* Read one byte from the CDC port; returns the byte (0-255) or -1 if none. */
int usb_read_byte(void);

/* Non-zero when a host has opened the CDC port (DTR asserted). */
int usb_connected(void);

/* Non-zero when a USB host has enumerated us (TinyUSB mounted). Distinguishes a
 * real PC data connection from a dumb charger that only supplies VBUS, so it is
 * the basis for the master/sensor role decision (DESIGN §8). */
int usb_host_present(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_H */
