#include "secure.h"

int sec_replay(sec_replay_t *st, uint64_t boot_id, uint32_t ctr)
{
    if (ctr == 0u || ctr > 0xffffffu) { return 0; }
    if (st->have && boot_id == st->boot_id) {
        if (ctr <= st->max_ctr) { return 0; }
    } else {
        for (unsigned i = 0; i < st->retired_count; i++) {
            if (st->retired[i] == boot_id) { return 0; }
        }
        if (st->have) {
            st->retired[st->retired_next] = st->boot_id;
            st->retired_next = (uint8_t)((st->retired_next + 1u) % SEC_RETIRED_BOOTS);
            if (st->retired_count < SEC_RETIRED_BOOTS) { st->retired_count++; }
        }
        st->have = 1;
        st->boot_id = boot_id;
    }
    st->max_ctr = ctr;
    return 1;
}
