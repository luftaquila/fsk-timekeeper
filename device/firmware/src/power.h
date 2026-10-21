/* Master USB power policy (pure logic): without VBUS the master stops (no beacons, no radio);
 * VBUS back reboots into a new session. A host merely suspending the bus keeps VBUS: no stop. */
#ifndef POWER_H
#define POWER_H

typedef enum { MP_RUN = 0, MP_STOPPED } mp_state_t;

#define MP_NONE  0
#define MP_STOP  1 /* enter the stopped state now */
#define MP_RESET 2 /* VBUS is back: reboot */

int mp_step(mp_state_t *state, int vbus_present);

#endif /* POWER_H */
