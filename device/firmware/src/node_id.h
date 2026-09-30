/* This board's identity: the factory-unique chip id (FICR.DEVICEID). Every board
 * runs the same binary; the role is decided from USB at boot (DESIGN §8), and a
 * sensor is registered by the master on its first authenticated uplink (§2.3). */
#ifndef NODE_ID_H
#define NODE_ID_H

#include <stdint.h>

/* Read FICR.DEVICEID. Call once at startup, before usb_init(). */
void node_init(void);

/* 64-bit chip id, reported on the I line and as the USB serial number. */
uint32_t node_devid_hi(void);
uint32_t node_devid_lo(void);

/* 32-bit on-air sender id: low word of the chip id, never 0 (the master's id). */
uint32_t node_sender_id(void);

#endif /* NODE_ID_H */
