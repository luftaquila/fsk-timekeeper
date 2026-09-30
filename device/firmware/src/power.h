/* Master USB power policy (W1): without VBUS the master stops (no beacons, no
 * radio); when VBUS returns it reboots into a new session. A host that merely
 * suspends the bus keeps VBUS and is not a stop. Pure logic. */
#ifndef POWER_H
#define POWER_H

typedef enum { MP_RUN = 0, MP_STOPPED } mp_state_t;

#define MP_NONE  0
#define MP_STOP  1 /* enter the stopped state now */
#define MP_RESET 2 /* VBUS is back: reboot */

int mp_step(mp_state_t *state, int vbus_present);

#endif /* POWER_H */
