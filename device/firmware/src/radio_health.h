/* Recovery history is separate from active radio faults. Counts saturate per
 * board boot; reason/result describe the latest attempt, not every past one. */
#ifndef RADIO_HEALTH_H
#define RADIO_HEALTH_H
#include <stdint.h>

enum {
    RADIO_RECOVERY_NONE, RADIO_RECOVERY_BOOT, RADIO_RECOVERY_BEACON_LOSS,
    RADIO_RECOVERY_SPI, RADIO_RECOVERY_CAD, RADIO_RECOVERY_CLOCK,
    RADIO_RECOVERY_RX, RADIO_RECOVERY_INIT, RADIO_RECOVERY_TX
};
enum { RADIO_RECOVERY_IDLE, RADIO_RECOVERY_PENDING, RADIO_RECOVERY_OK, RADIO_RECOVERY_FAILED };
typedef struct __attribute__((packed)) {
    uint16_t count;
    uint16_t failures;
    uint8_t state; /* low nibble: reason; high nibble: result (explicit wire packing) */
} radio_recovery_diag_t;
/* Six cumulative failure counters, then the latest recovery summary. This fits
 * the checkpoint's existing 17-byte union; never appended to every uplink. */
#define RADIO_HISTORY_FAULTS 6u
typedef struct __attribute__((packed)) {
    uint16_t faults[RADIO_HISTORY_FAULTS]; /* CAD, SPI, TX, RX, clock, init */
    radio_recovery_diag_t recovery;
} radio_history_t;
#define RADIO_RECOVERY_REASON(d) ((d).state & 0x0Fu)
#define RADIO_RECOVERY_RESULT(d) ((d).state >> 4)
#endif
