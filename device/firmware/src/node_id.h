/* This board's stable identity is the nRF52840 factory-unique chip id
 * (FICR.DEVICEID), so every board runs the SAME binary. Role is NOT baked in: it
 * is decided at runtime from USB (master = the node a PC host connects to over
 * USB-CDC, else sensor — DESIGN §8). A sensor transmits under its own 32-bit
 * chip id and the master auto-registers it on first authenticated contact
 * (DESIGN §2.3). Adding a sensor = plug it in / power it on; no table to edit.
 */
#ifndef NODE_ID_H
#define NODE_ID_H

#include <stdint.h>

/* Read FICR.DEVICEID into this module. Call once at startup. */
void node_init(void);

float node_freq_mhz(void);  /* the single shared channel frequency */

/* Raw 64-bit chip id — the full identity reported over USB on the I line. */
uint32_t node_devid_hi(void);
uint32_t node_devid_lo(void);

/* The 32-bit on-air sender identity for this board (low 32 bits of the chip id,
 * forced nonzero so it never collides with the master's reserved id 0). */
uint32_t node_sender_id(void);

#endif /* NODE_ID_H */
