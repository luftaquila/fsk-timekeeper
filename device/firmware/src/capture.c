#include "capture.h"

#include "config.h"
#include "board.h"
#include "gpio.h"
#include "nrf.h"

/* TIMER1 = capture timebase (TIMER0 = unused SD, TIMER2 = board_micros).
 * SoftDevice never enabled so all GPIOTE/PPI channels are free. */
#define CAP_GPIOTE_DIO1 0
#define CAP_GPIOTE_SENS 1
#define CAP_PPI_DIO1    0
#define CAP_PPI_SENS    1
#define CAP_PPI_USB_SOF 2
#define CAP_CC_DIO1     0 /* TIMER1->CC[] holding DIO1 captures */
#define CAP_CC_NOW      1 /* TIMER1->CC[] for on-demand reads */
#define CAP_CC_SENS     2 /* TIMER1->CC[] holding SENSOR captures */
#define CAP_CC_USB_SOF  3 /* TIMER1->CC[] holding the latest USB SOF */
#define SENSOR_QUEUE_LEN 16u

#define DIO1_PIN (PIN_LORA_DIO1 % 32u) /* 6 */
#define DIO1_PRT (PIN_LORA_DIO1 / 32u) /* 1 */
#define SENS_PIN (PIN_SENSOR_IN % 32u) /* 11 */
#define SENS_PRT (PIN_SENSOR_IN / 32u) /* 1 */

static volatile uint32_t s_sensor_queue[SENSOR_QUEUE_LEN];
static volatile uint32_t s_sensor_seq_queue[SENSOR_QUEUE_LEN];
static volatile uint8_t s_sensor_clock_queue[SENSOR_QUEUE_LEN];
static volatile uint32_t s_sensor_seq;
static volatile uint32_t s_loss_first_seq, s_loss_last_seq, s_loss_first_tick, s_loss_last_tick;
static volatile int s_loss_pending;
static volatile uint8_t s_sensor_head;
static volatile uint8_t s_sensor_tail;
static volatile uint16_t s_sensor_overflow;

void GPIOTE_IRQHandler(void)
{
    if (NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS]) {
        NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
        uint32_t seq = ++s_sensor_seq;
        uint8_t head = s_sensor_head;
        uint8_t next = (uint8_t)((head + 1u) & (SENSOR_QUEUE_LEN - 1u));
        if (next == s_sensor_tail) {
            if (s_sensor_overflow != UINT16_MAX) { s_sensor_overflow++; }
            if (!s_loss_pending) { s_loss_first_seq = seq; s_loss_first_tick = NRF_TIMER1->CC[CAP_CC_SENS]; }
            s_loss_last_seq = seq; s_loss_last_tick = NRF_TIMER1->CC[CAP_CC_SENS]; s_loss_pending = 1;
        } else {
            s_sensor_queue[head] = NRF_TIMER1->CC[CAP_CC_SENS];
            s_sensor_seq_queue[head] = seq;
            s_sensor_clock_queue[head] = board_hfclk_xtal();
            __DMB();
            s_sensor_head = next;
        }
    }
}

void capture_init(void)
{
    NRF_TIMER1->TASKS_STOP = 1;
    NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER1->PRESCALER = 0; /* 16 MHz, 62.5 ns/tick */
    NRF_TIMER1->TASKS_CLEAR = 1;
    NRF_TIMER1->TASKS_START = 1;

    /* DIO1 (Tx/RxDone) — rising edge. */
    NRF_GPIOTE->CONFIG[CAP_GPIOTE_DIO1] =
        ((uint32_t)GPIOTE_CONFIG_MODE_Event       << GPIOTE_CONFIG_MODE_Pos)     |
        ((uint32_t)DIO1_PIN                       << GPIOTE_CONFIG_PSEL_Pos)     |
        ((uint32_t)DIO1_PRT                       << GPIOTE_CONFIG_PORT_Pos)     |
        ((uint32_t)GPIOTE_CONFIG_POLARITY_LoToHi  << GPIOTE_CONFIG_POLARITY_Pos);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1] = 0;
    NRF_PPI->CH[CAP_PPI_DIO1].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1];
    NRF_PPI->CH[CAP_PPI_DIO1].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_DIO1];

    /* SENSOR (event) — falling edge (NPN open-collector); needs the pull-up. */
    gpio_cfg_input_pullup(PIN_SENSOR_IN);
    NRF_GPIOTE->CONFIG[CAP_GPIOTE_SENS] =
        ((uint32_t)GPIOTE_CONFIG_MODE_Event       << GPIOTE_CONFIG_MODE_Pos)     |
        ((uint32_t)SENS_PIN                       << GPIOTE_CONFIG_PSEL_Pos)     |
        ((uint32_t)SENS_PRT                       << GPIOTE_CONFIG_PORT_Pos)     |
        ((uint32_t)GPIOTE_CONFIG_POLARITY_HiToLo  << GPIOTE_CONFIG_POLARITY_Pos);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
    NRF_PPI->CH[CAP_PPI_SENS].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    NRF_PPI->CH[CAP_PPI_SENS].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_SENS];

    NRF_PPI->CHENSET = (1UL << CAP_PPI_DIO1) | (1UL << CAP_PPI_SENS);

    NRF_GPIOTE->INTENSET = (1UL << (GPIOTE_INTENSET_IN0_Pos + CAP_GPIOTE_SENS));
    NVIC_ClearPendingIRQ(GPIOTE_IRQn);
    NVIC_SetPriority(GPIOTE_IRQn, 3);
    NVIC_EnableIRQ(GPIOTE_IRQn);
}

/* 32->64-bit extension. capture_now64() polls the 32-bit counter and bumps the
 * high word on wrap; it must be called more often than the ~268 s wrap period. */
static uint64_t s_base; /* accumulated multiples of 2^32 */
static uint32_t s_prev; /* last low word observed */

static uint32_t timer1_now32(void)
{
    NRF_TIMER1->TASKS_CAPTURE[CAP_CC_NOW] = 1;
    return NRF_TIMER1->CC[CAP_CC_NOW];
}

uint64_t capture_now64(void)
{
    uint32_t low = timer1_now32();
    if (low < s_prev) {
        s_base += (uint64_t)1 << 32;
    }
    s_prev = low;
    return s_base + low;
}

/* Widen a recently-latched 32-bit capture (< 2^32 ticks ago) to 64 bits. */
static uint64_t widen(uint32_t cap_low)
{
    uint64_t now = capture_now64();
    uint32_t delta = (uint32_t)now - cap_low; /* ticks since capture (mod 2^32) */
    return now - delta;
}

int capture_dio1_get(uint64_t *tick)
{
    if (NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1] == 0) {
        return 0;
    }
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1] = 0;
    *tick = widen(NRF_TIMER1->CC[CAP_CC_DIO1]);
    return 1;
}

int capture_sensor_get(uint64_t *tick, uint32_t *seq, int *clock_xtal)
{
    uint8_t tail = s_sensor_tail;
    if (tail == s_sensor_head) { return 0; }
    uint32_t low = s_sensor_queue[tail];
    *seq = s_sensor_seq_queue[tail];
    *clock_xtal = s_sensor_clock_queue[tail];
    __DMB();
    s_sensor_tail = (uint8_t)((tail + 1u) & (SENSOR_QUEUE_LEN - 1u));
    *tick = widen(low);
    return 1;
}

int capture_sensor_loss(uint64_t *first_tick, uint64_t *last_tick, uint32_t *first_seq, uint32_t *last_seq)
{
    NVIC_DisableIRQ(GPIOTE_IRQn);
    if (!s_loss_pending) { NVIC_EnableIRQ(GPIOTE_IRQn); return 0; }
    *first_tick = widen(s_loss_first_tick); *last_tick = widen(s_loss_last_tick);
    *first_seq = s_loss_first_seq; *last_seq = s_loss_last_seq;
    s_loss_pending = 0;
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return 1;
}

int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq)
{
    NVIC_DisableIRQ(GPIOTE_IRQn);
    *tick = capture_now64();
    int empty = s_sensor_head == s_sensor_tail && !s_loss_pending && !NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    *seq = s_sensor_seq;
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return empty;
}

uint16_t capture_sensor_overflow(void)
{
    return s_sensor_overflow;
}

void capture_usb_sof_enable(void)
{
    NRF_PPI->CH[CAP_PPI_USB_SOF].EEP = (uint32_t)&NRF_USBD->EVENTS_SOF;
    NRF_PPI->CH[CAP_PPI_USB_SOF].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_USB_SOF];
    NRF_PPI->CHENSET = (1UL << CAP_PPI_USB_SOF);
}

int capture_usb_sof_sample(uint64_t *tick, uint16_t *frame)
{
    if (!(NRF_USBD->ENABLE & USBD_ENABLE_ENABLE_Msk)) { return 0; }
    uint16_t first;
    uint16_t second;
    uint32_t low;
    do {
        first = (uint16_t)NRF_USBD->FRAMECNTR;
        low = NRF_TIMER1->CC[CAP_CC_USB_SOF];
        second = (uint16_t)NRF_USBD->FRAMECNTR;
    } while (first != second);
    *frame = first;
    *tick = widen(low);
    return 1;
}
