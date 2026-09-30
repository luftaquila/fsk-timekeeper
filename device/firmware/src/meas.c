#include "meas.h"

#include "board.h"
#include "errlog.h"
#include "nrf.h"

/* Every wait here finishes in well under 1 ms. */
#define MEAS_WAIT_MAX_US 2000u

static int wait_event(volatile uint32_t *event)
{
    uint32_t t0 = board_micros();
    while (*event == 0) {
        if ((uint32_t)(board_micros() - t0) >= MEAS_WAIT_MAX_US) {
            el_note(EL_HW_TIMEOUT);
            return 0;
        }
    }
    return 1;
}

int16_t meas_temp_c10(void)
{
    NRF_TEMP->EVENTS_DATARDY = 0;
    NRF_TEMP->TASKS_START = 1;
    int ok = wait_event(&NRF_TEMP->EVENTS_DATARDY);
    NRF_TEMP->EVENTS_DATARDY = 0;
    int32_t raw = (int32_t)NRF_TEMP->TEMP; /* signed, 0.25 C units */
    NRF_TEMP->TASKS_STOP = 1;
    return ok ? (int16_t)((raw * 10) / 4) : 0;
}

/* EasyDMA target for the single sample. */
static volatile int16_t s_adc_result;

uint16_t meas_vddh_mv(void)
{
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
    NRF_SAADC->CH[0].CONFIG =
        ((uint32_t)SAADC_CH_CONFIG_GAIN_Gain1_6   << SAADC_CH_CONFIG_GAIN_Pos)   |
        ((uint32_t)SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
        ((uint32_t)SAADC_CH_CONFIG_TACQ_10us      << SAADC_CH_CONFIG_TACQ_Pos)   |
        ((uint32_t)SAADC_CH_CONFIG_MODE_SE        << SAADC_CH_CONFIG_MODE_Pos)   |
        ((uint32_t)SAADC_CH_CONFIG_RESP_Bypass    << SAADC_CH_CONFIG_RESP_Pos)   |
        ((uint32_t)SAADC_CH_CONFIG_RESN_Bypass    << SAADC_CH_CONFIG_RESN_Pos);
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDDHDIV5;
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;

    s_adc_result = 0;
    NRF_SAADC->RESULT.PTR = (uint32_t)&s_adc_result;
    NRF_SAADC->RESULT.MAXCNT = 1;

    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
    NRF_SAADC->EVENTS_STARTED = 0;
    NRF_SAADC->TASKS_START = 1;
    int ok = wait_event(&NRF_SAADC->EVENTS_STARTED);
    if (ok) {
        NRF_SAADC->EVENTS_END = 0;
        NRF_SAADC->TASKS_SAMPLE = 1;
        ok = wait_event(&NRF_SAADC->EVENTS_END);
    }
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->TASKS_STOP = 1;
    (void)wait_event(&NRF_SAADC->EVENTS_STOPPED);
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
    if (!ok) { return 0; }

    int32_t code = s_adc_result;
    if (code < 0) { code = 0; } /* single-ended; clamp noise below 0 */

    /* SE 12-bit, ref 0.6 V, gain 1/6 -> 3.6 V full scale; input = VDDH / 5. */
    return (uint16_t)(((uint32_t)code * 18000u) / 4096u);
}
