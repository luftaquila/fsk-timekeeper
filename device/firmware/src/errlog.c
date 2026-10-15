#include "errlog.h"

#include "proto_usb.h"
#include "usb.h"

static const struct {
    const char *name;
    uint16_t flag;
} k_code[EL_COUNT] = {
    [EL_RADIO_RESET]   = { "radio_reset",   0 },
    [EL_CAD_TIMEOUT]   = { "cad_timeout",   ERR_CAD_TIMEOUT },
    [EL_SPI_TIMEOUT]   = { "spi_timeout",   ERR_SPI_TIMEOUT },
    [EL_TX_FAIL]       = { "tx_fail",       ERR_TX_FAIL },
    [EL_RX_FAIL]       = { "rx_fail",       ERR_RX_FAIL },
    [EL_BEACON_FAIL]   = { "beacon_fail",   ERR_TX_FAIL },
    [EL_HFXO_RESTART]  = { "hfxo_restart",  ERR_HFXO_RESTART },
    [EL_HW_TIMEOUT]    = { "hw_timeout",    ERR_HW_TIMEOUT },
    [EL_QUEUE_FULL]    = { "queue_full",    ERR_QUEUE_FULL },
    [EL_USB_DROP]      = { "usb_drop",      ERR_USB_DROP },
    [EL_ACK_GAP]       = { "ack_gap",       ERR_ACK },
    [EL_ACK_MISMATCH]  = { "ack_mismatch",  ERR_ACK },
    [EL_ID_COLLISION]  = { "id_collision",  ERR_ID_COLLISION },
    [EL_REGISTRY_FULL] = { "registry_full", 0 },
    [EL_FIFO_FULL]     = { "fifo_full",     ERR_FIFO_FULL },
    [EL_RADIO_CLOCK]   = { "radio_clock",   ERR_RADIO_CLOCK },
    [EL_RADIO_INIT]    = { "radio_init",    ERR_RADIO_INIT },
};

static uint32_t s_count[EL_COUNT];
static uint32_t s_reported[EL_COUNT];
static uint32_t s_report_ms[EL_COUNT];
static uint16_t s_flags;
static radio_recovery_diag_t s_recovery;

void el_note(el_code_t code)
{
    if ((unsigned)code >= EL_COUNT) { return; }
    if (s_count[code] != UINT32_MAX) { s_count[code]++; }
    s_flags |= k_code[code].flag;
}

uint32_t el_count(el_code_t code)
{
    return (unsigned)code < EL_COUNT ? s_count[code] : 0;
}

uint16_t el_flags(void)
{
    return s_flags;
}

void el_radio_clear(el_code_t code)
{
    switch (code) {
    case EL_CAD_TIMEOUT: case EL_SPI_TIMEOUT: case EL_TX_FAIL: case EL_RX_FAIL:
    case EL_RADIO_CLOCK: case EL_RADIO_INIT:
        s_flags &= (uint16_t)~k_code[code].flag;
        break;
    default: break;
    }
}

uint8_t el_radio_recovery_reason(void)
{
    if (s_flags & ERR_RADIO_CLOCK) { return RADIO_RECOVERY_CLOCK; }
    if (s_flags & ERR_SPI_TIMEOUT) { return RADIO_RECOVERY_SPI; }
    if (s_flags & ERR_CAD_TIMEOUT) { return RADIO_RECOVERY_CAD; }
    if (s_flags & ERR_RX_FAIL) { return RADIO_RECOVERY_RX; }
    if (s_flags & ERR_TX_FAIL) { return RADIO_RECOVERY_TX; }
    return RADIO_RECOVERY_INIT;
}

void el_radio_recovery_begin(uint8_t reason)
{
    el_note(EL_RADIO_RESET);
    if (s_recovery.count != UINT16_MAX) { s_recovery.count++; }
    s_recovery.state = reason | (RADIO_RECOVERY_PENDING << 4);
}

void el_radio_recovery_end(int ok)
{
    s_recovery.state = RADIO_RECOVERY_REASON(s_recovery) | ((ok ? RADIO_RECOVERY_OK : RADIO_RECOVERY_FAILED) << 4);
    if (!ok) {
        if (s_recovery.failures != UINT16_MAX) { s_recovery.failures++; }
        el_note(EL_RADIO_INIT);
    }
}

radio_recovery_diag_t el_radio_recovery(void) { return s_recovery; }

radio_history_t el_radio_history(void)
{
    static const el_code_t codes[RADIO_HISTORY_FAULTS] = {
        EL_CAD_TIMEOUT, EL_SPI_TIMEOUT, EL_TX_FAIL, EL_RX_FAIL, EL_RADIO_CLOCK, EL_RADIO_INIT
    };
    radio_history_t d = { .recovery = s_recovery };
    for (unsigned i = 0; i < RADIO_HISTORY_FAULTS; i++) {
        uint32_t n = el_count(codes[i]);
        d.faults[i] = (uint16_t)(n > UINT16_MAX ? UINT16_MAX : n);
    }
    return d;
}

void el_report(uint32_t now_ms)
{
    if (!usb_console_ready()) { return; }
    for (unsigned i = 0; i < EL_COUNT; i++) {
        if (i == EL_RADIO_RESET) { continue; } /* reported with reason/result in diagnostics */
        if (s_count[i] == s_reported[i]) { continue; }
        if (s_reported[i] != 0 && (uint32_t)(now_ms - s_report_ms[i]) < 1000u) { continue; }
        s_reported[i] = s_count[i];
        s_report_ms[i] = now_ms;
        pu_emit_err_count(k_code[i].name, s_count[i]);
    }
}
