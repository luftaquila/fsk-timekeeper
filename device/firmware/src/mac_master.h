/* Master MAC: a beacon every BEACON_PERIOD_MS (slot table + cumulative ACKs), then data or
 * admission in the scheduled slots. Checks transport integrity only (auth, session, replay,
 * order); judging the evidence is the console's job. Main-loop context. */
#ifndef MAC_MASTER_H
#define MAC_MASTER_H

#include <stdint.h>
#include "retry.h"

#include "master_queue.h"
#include "registry.h"
#include "master_admission.h"

typedef enum { MASTER_TX_IDLE = 0, MASTER_TX_BEACON, MASTER_TX_OFFER } master_tx_state_t;

typedef struct {
    registry_t reg;
    mq_t *q;
    uint64_t next_beacon;  /* TIMER1 tick of the next beacon start */
    uint8_t  seq;
    int      m_tx_valid;
    uint64_t m_tx_last;    /* TxDone of the last beacon */
    uint8_t  cp_id;
    unsigned cp_left;
    uint32_t auth_drop;    /* unknown/unlinked sender, or failed AEAD/boot ID check */
    uint32_t ver_drop;     /* packets of another radio protocol version */
    uint32_t last_report_ms; /* full registry status refresh over USB */
    retry_t retry;
    master_admission_t admission;
    uint64_t cycle_tx;
    uint32_t beacon_ctr;
    uint64_t reply_rx;
    uint32_t reply_link;
    uint16_t reply_age_ms;
    uint8_t cycle_seq, cycle_wire_len, reply_entry, reply_valid;
    master_tx_state_t tx_state;
    uint8_t schedule[SLOT_COUNT], cycle_ready, slot_done;
} mac_master_t;

void mac_master_init(mac_master_t *m, mq_t *q);

/* One main-loop pass; returns 1 when a beacon went out. */
int mac_master_step(mac_master_t *m);

/* Host `CP`: the next CP_REQ_BEACONS beacons ask every sensor for a checkpoint. */
void mac_master_checkpoint(mac_master_t *m);

/* A new master session after sec_init(). */
void mac_master_session(mac_master_t *m);

/* D and J lines of the registered sensors (J WAIT for candidates on a full report). ?STATUS asks
 * for all; changed_only (beacon loop): transitions at once, all every SENSOR_STATUS_PERIOD_MS. */
void mac_master_report(mac_master_t *m, int changed_only);

#endif /* MAC_MASTER_H */
