#include "power.h"

int mp_step(mp_state_t *state, int vbus_present)
{
    if (*state == MP_RUN) {
        if (vbus_present) { return MP_NONE; }
        *state = MP_STOPPED;
        return MP_STOP;
    }
    return vbus_present ? MP_RESET : MP_NONE;
}
