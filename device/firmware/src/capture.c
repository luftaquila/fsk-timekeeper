#include "capture.h"

#include "config.h"
#include "board.h"
#include "gpio.h"
#include "nrf.h"

/* TIMER1 = capture timebase (TIMER2 = board_micros). The SoftDevice is never
 * enabled, so every GPIOTE/PPI channel is free. */
#define CAP_GPIOTE_DIO1 0
#define CAP_GPIOTE_SENS 1
#define CAP_GPIOTE_PPS  2
#define CAP_PPI_DIO1    0
#define CAP_PPI_SENS    1
#define CAP_PPI_PPS     2
#define CAP_CC_DIO1     0 /* TIMER1->CC[] latched by DIO1 */
#define CAP_CC_NOW      1 /* TIMER1->CC[] for on-demand reads */
#define CAP_CC_SENS     2 /* TIMER1->CC[] latched by SENSOR */
#define CAP_CC_PPS      3 /* TIMER1->CC[] latched by PPS */
#define SENSOR_RING_LEN 64u /* power of two */

#define DIO1_PIN (PIN_LORA_DIO1 % 32u)
#define DIO1_PRT (PIN_LORA_DIO1 / 32u)
#define SENS_PIN (PIN_SENSOR_IN % 32u)
#define SENS_PRT (PIN_SENSOR_IN / 32u)
#define PPS_PIN  (PIN_GPS_PPS % 32u)
#define PPS_PRT  (PIN_GPS_PPS / 32u)

static volatile uint32_t s_ring_tick[SENSOR_RING_LEN];
static volatile uint32_t s_ring_seq[SENSOR_RING_LEN];
static volatile uint8_t s_ring_xtal[SENSOR_RING_LEN];
static volatile uint32_t s_sensor_seq;
static volatile uint32_t s_loss_first_seq, s_loss_last_seq, s_loss_first_tick, s_loss_last_tick;
static volatile int s_loss_pending;
static volatile uint8_t s_head;
static volatile uint8_t s_tail;
static volatile uint16_t s_overflow;

/* SENSOR edge: ring the latched tick; when the ring is full, extend the loss range.
 * A pending range keeps growing until the main loop takes it, even once the ring
 * has room again, so every ring item precedes it and records leave in seq order. */
void GPIOTE_IRQHandler(void)
{
    if (NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS]) {
        NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
        uint32_t seq = ++s_sensor_seq;
        uint32_t tick = NRF_TIMER1->CC[CAP_CC_SENS];
        uint8_t head = s_head;
        uint8_t next = (uint8_t)((head + 1u) & (SENSOR_RING_LEN - 1u));
        if (next == s_tail || s_loss_pending) {
            if (s_overflow != UINT16_MAX) { s_overflow++; }
            if (!s_loss_pending) { s_loss_first_seq = seq; s_loss_first_tick = tick; }
            s_loss_last_seq = seq; s_loss_last_tick = tick; s_loss_pending = 1;
        } else {
            s_ring_tick[head] = tick;
            s_ring_seq[head] = seq;
            s_ring_xtal[head] = (uint8_t)board_hfclk_xtal();
            __DMB();
            s_head = next;
        }
    }
}

static uint32_t gpiote_event(uint32_t pin, uint32_t port, uint32_t polarity)
{
    return ((uint32_t)GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
           (pin << GPIOTE_CONFIG_PSEL_Pos) | (port << GPIOTE_CONFIG_PORT_Pos) |
           (polarity << GPIOTE_CONFIG_POLARITY_Pos);
}

void capture_init(void)
{
    NRF_TIMER1->TASKS_STOP = 1;
    NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER1->PRESCALER = 0; /* 16 MHz */
    NRF_TIMER1->TASKS_CLEAR = 1;
    NRF_TIMER1->TASKS_START = 1;

    NRF_GPIOTE->CONFIG[CAP_GPIOTE_DIO1] = gpiote_event(DIO1_PIN, DIO1_PRT, GPIOTE_CONFIG_POLARITY_LoToHi);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1] = 0;
    NRF_PPI->CH[CAP_PPI_DIO1].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_DIO1];
    NRF_PPI->CH[CAP_PPI_DIO1].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_DIO1];
    NRF_PPI->CHENSET = (1UL << CAP_PPI_DIO1);
}

void capture_sensor_enable(void)
{
    gpio_cfg_input_pullup(PIN_SENSOR_IN); /* NPN open collector */
    NRF_GPIOTE->CONFIG[CAP_GPIOTE_SENS] = gpiote_event(SENS_PIN, SENS_PRT, GPIOTE_CONFIG_POLARITY_HiToLo);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
    NRF_PPI->CH[CAP_PPI_SENS].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    NRF_PPI->CH[CAP_PPI_SENS].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_SENS];
    NRF_PPI->CHENSET = (1UL << CAP_PPI_SENS);

    NRF_GPIOTE->INTENSET = (1UL << (GPIOTE_INTENSET_IN0_Pos + CAP_GPIOTE_SENS));
    NVIC_ClearPendingIRQ(GPIOTE_IRQn);
    NVIC_SetPriority(GPIOTE_IRQn, 3);
    NVIC_EnableIRQ(GPIOTE_IRQn);
}

void capture_pps_enable(void)
{
    /* Rising edge (the ATGM336H aligns it to the UTC second); polled like DIO1.
     * Pull-down: an unpopulated GPS gives no edges. */
    gpio_cfg_input_pulldown(PIN_GPS_PPS);
    NRF_GPIOTE->CONFIG[CAP_GPIOTE_PPS] = gpiote_event(PPS_PIN, PPS_PRT, GPIOTE_CONFIG_POLARITY_LoToHi);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_PPS] = 0;
    NRF_PPI->CH[CAP_PPI_PPS].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_PPS];
    NRF_PPI->CH[CAP_PPI_PPS].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_PPS];
    NRF_PPI->CHENSET = (1UL << CAP_PPI_PPS);
}

/* 32 -> 64-bit extension: bump the high word whenever the low word wrapped. */
static uint64_t s_base;
static uint32_t s_prev;

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

/* Widen a capture latched less than 2^32 ticks ago. */
static uint64_t widen(uint32_t cap_low)
{
    uint64_t now = capture_now64();
    uint32_t delta = (uint32_t)now - cap_low;
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
    uint8_t tail = s_tail;
    if (tail == s_head) { return 0; }
    uint32_t low = s_ring_tick[tail];
    *seq = s_ring_seq[tail];
    *clock_xtal = s_ring_xtal[tail];
    __DMB();
    s_tail = (uint8_t)((tail + 1u) & (SENSOR_RING_LEN - 1u));
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
    int empty = s_head == s_tail && !s_loss_pending && !NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    *seq = s_sensor_seq;
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return empty;
}

uint16_t capture_sensor_overflow(void)
{
    return s_overflow;
}

int capture_pps_get(uint64_t *tick)
{
    if (NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_PPS] == 0) {
        return 0;
    }
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_PPS] = 0;
    *tick = widen(NRF_TIMER1->CC[CAP_CC_PPS]);
    return 1;
}
