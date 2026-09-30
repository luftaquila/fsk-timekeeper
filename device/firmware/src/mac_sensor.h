/* Sensor MAC (DESIGN §2.8): sync to beacons, stamp captures, and send at
 * most one packet per beacon in the sensor's slot; the beacon's slot table
 * carries the cumulative ACK. A sensor that heard no beacon this cycle stays
 * silent. Main-loop context. */
#ifndef MAC_SENSOR_H
#define MAC_SENSOR_H

#include <stdint.h>

#include "protocol.h"
#include "secure.h"
#include "sensor_queue.h"
#include "sync.h"

typedef struct {
    uint32_t my_id;
    uint16_t short_id;
    uint8_t  boot_tag;       /* low byte of this sensor's boot_id */
    uint8_t  reset_reason;
    sync_t sync;
    sensor_queue_t q;
    sec_replay_t from_master;
    int      have_session;
    uint32_t master_boot_id;
    int      timing_ready;   /* health was complete once in this session */
    uint64_t timing_ready_tick;
    /* current beacon cycle */
    int      cycle;          /* a beacon with a captured RxDone arrived and its slot is ahead */
    uint64_t cycle_rx;       /* its RxDone, local ticks */
    uint8_t  cycle_seq;
    int      slot;           /* own slot in the table, -1 = not registered */
    int      tx_slot;        /* slot to use this cycle, -1 = stay silent */
    uint8_t  cp_pending, cp_answered;
    uint32_t last_beacon_ms;
    uint32_t reset_after_ms; /* earliest next radio reset */
    uint32_t reset_backoff_ms;
    uint32_t hfxo_stops;     /* board_hfxo_stops() already handled */
} mac_sensor_t;

void mac_sensor_init(mac_sensor_t *s, uint32_t my_id, uint8_t reset_reason);
void mac_sensor_step(mac_sensor_t *s);

#endif /* MAC_SENSOR_H */
