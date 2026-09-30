#include "errlog.h"

#include "proto_usb.h"

static const struct {
    const char *name;
    uint16_t flag;
} k_code[EL_COUNT] = {
    [EL_RADIO_RESET]   = { "radio_reset",   ERR_RADIO_RESET },
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
};

static uint32_t s_count[EL_COUNT];
static uint32_t s_reported[EL_COUNT];
static uint32_t s_report_ms[EL_COUNT];
static uint16_t s_flags;

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

void el_report(uint32_t now_ms)
{
    for (unsigned i = 0; i < EL_COUNT; i++) {
        if (s_count[i] == s_reported[i]) { continue; }
        if (s_reported[i] != 0 && (uint32_t)(now_ms - s_report_ms[i]) < 1000u) { continue; }
        s_reported[i] = s_count[i];
        s_report_ms[i] = now_ms;
        pu_emit_err_count(k_code[i].name, s_count[i]);
    }
}
