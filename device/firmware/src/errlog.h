/* Error counters. Every failure path counts here; the master reports changes as
 * `X <code> <count>` lines (at most once per second per code) and every node
 * reports active radio faults and sticky non-radio ERR_* bits in diagnostics. Main loop
 * only (not ISR safe). */
#ifndef ERRLOG_H
#define ERRLOG_H

#include <stdint.h>
#include "radio_health.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EL_RADIO_RESET,
    EL_CAD_TIMEOUT,
    EL_SPI_TIMEOUT,
    EL_TX_FAIL,
    EL_RX_FAIL,
    EL_BEACON_FAIL,
    EL_HFXO_RESTART,
    EL_HW_TIMEOUT,
    EL_QUEUE_FULL,
    EL_USB_DROP,
    EL_ACK_GAP,
    EL_ACK_MISMATCH,
    EL_REGISTRY_FULL,
    EL_FIFO_FULL,
    EL_RADIO_CLOCK,
    EL_RADIO_INIT,
    EL_COUNT
} el_code_t;

void el_note(el_code_t code);
uint32_t el_count(el_code_t code);
uint16_t el_flags(void);
/* Only reversible radio faults may be cleared; counters always retain history. */
void el_radio_clear(el_code_t code);
uint8_t el_radio_recovery_reason(void);
void el_radio_recovery_begin(uint8_t reason);
void el_radio_recovery_end(int ok);
radio_recovery_diag_t el_radio_recovery(void);
radio_history_t el_radio_history(void);

/* Emit X lines for codes whose count changed since their last report. */
void el_report(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ERRLOG_H */
