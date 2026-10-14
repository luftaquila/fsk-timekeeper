/* Sensor MAC (DESIGN §2.8): sync to beacons, stamp captures, and send at
 * most one packet per beacon, in the first of the sensor's slots it is ready
 * for; the beacon's slot table carries the cumulative ACK. A sensor that heard
 * no beacon this cycle stays silent. Main-loop context. */
#ifndef MAC_SENSOR_H
#define MAC_SENSOR_H

#include <stdint.h>
#include "retry.h"

#include "protocol.h"
#include "secure.h"
#include "sensor_queue.h"
#include "sync.h"

typedef struct {
    uint32_t my_id;
    uint8_t  reset_reason;
    sync_t sync;
    sensor_queue_t q;
    sec_replay_t from_master;
    int      have_session;
    uint64_t master_boot_id;
    int      timing_ready;   /* health was complete once in this session */
    uint64_t timing_ready_tick;
    /* current beacon cycle */
    uint64_t cycle_rx;       /* its RxDone, local ticks */
    uint8_t  cycle_seq;
    uint8_t  tx_slots;       /* bit j: slot j is ours and still ahead; 0 once a packet went out */
    int      entry;          /* own entry in the slot table, -1 = not registered */
    uint8_t  cp_pending, cp_answered;
    uint32_t last_beacon_ms;
    retry_t retry;
    uint32_t hfxo_stops;     /* board_hfxo_stops() already handled */
} mac_sensor_t;

void mac_sensor_init(mac_sensor_t *s, uint32_t my_id, uint8_t reset_reason);
void mac_sensor_step(mac_sensor_t *s);

#endif /* MAC_SENSOR_H */
