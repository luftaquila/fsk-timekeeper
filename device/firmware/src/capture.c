#include "capture.h"

#include "config.h"
#include "board.h"
#include "gpio.h"
#include "nrf.h"

/* TIMER1 timebase, TIMER2 micros, TIMER3 edge count; SoftDevice never on: all GPIOTE/PPI free. */
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
static volatile int s_loss_pending, s_loss_unknown;
static volatile uint8_t s_head;
static volatile uint8_t s_tail;
static volatile uint16_t s_overflow;

static uint32_t sensor_count(void)
{
#ifdef CAPTURE_HOST_TEST
    extern uint32_t capture_test_count(void);
    return capture_test_count();
#else
    NRF_TIMER3->TASKS_CAPTURE[0] = 1;
    return NRF_TIMER3->CC[0];
#endif
}

/* Every edge is counted even while the CPU stalls; pair tick and seq only on a stable snapshot. */
void GPIOTE_IRQHandler(void)
{
    if (!NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS]) { return; }
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
    uint32_t before = sensor_count();
    uint32_t tick = NRF_TIMER1->CC[CAP_CC_SENS];
    uint32_t seq = sensor_count();
    uint32_t delta = seq - s_sensor_seq;
    if (delta == 0u) { return; } /* a later edge already included by the previous ISR */
    int unknown = delta != 1u || before != seq || NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    uint32_t first = s_sensor_seq + 1u;
    s_sensor_seq = seq;
    uint8_t head = s_head;
    uint8_t next = (uint8_t)((head + 1u) & (SENSOR_RING_LEN - 1u));
    if (unknown || next == s_tail || s_loss_pending) {
        if (delta > (uint32_t)(UINT16_MAX - s_overflow)) { s_overflow = UINT16_MAX; }
        else { s_overflow = (uint16_t)(s_overflow + delta); }
        if (!s_loss_pending) {
            s_loss_first_seq = first;
            s_loss_first_tick = tick;
            s_loss_unknown = 0;
        }
        s_loss_last_seq = seq;
        s_loss_last_tick = tick;
        s_loss_unknown |= unknown;
        s_loss_pending = 1;
    } else {
        s_ring_tick[head] = tick;
        s_ring_seq[head] = seq;
        s_ring_xtal[head] = (uint8_t)board_hfclk_xtal();
        __DMB();
        s_head = next;
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

/* SENSOR falling edge -> TIMER1 CC and TIMER3 count on one PPI channel and its fork. */
static void sensor_edge_hw(void)
{
    NRF_TIMER3->TASKS_STOP = 1;
    NRF_TIMER3->MODE = TIMER_MODE_MODE_Counter;
    NRF_TIMER3->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER3->TASKS_CLEAR = 1;
    NRF_TIMER3->TASKS_START = 1;
    gpio_cfg_input_pullup(PIN_SENSOR_IN); /* NPN open collector */
    NRF_GPIOTE->CONFIG[CAP_GPIOTE_SENS] = gpiote_event(SENS_PIN, SENS_PRT, GPIOTE_CONFIG_POLARITY_HiToLo);
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
    NRF_PPI->CH[CAP_PPI_SENS].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    NRF_PPI->CH[CAP_PPI_SENS].TEP = (uint32_t)&NRF_TIMER1->TASKS_CAPTURE[CAP_CC_SENS];
    NRF_PPI->FORK[CAP_PPI_SENS].TEP = (uint32_t)&NRF_TIMER3->TASKS_COUNT;
    NRF_PPI->CHENSET = (1UL << CAP_PPI_SENS);
}

void capture_sensor_enable(void)
{
    sensor_edge_hw();
    NRF_GPIOTE->INTENSET = (1UL << (GPIOTE_INTENSET_IN0_Pos + CAP_GPIOTE_SENS));
    NVIC_ClearPendingIRQ(GPIOTE_IRQn);
    NVIC_SetPriority(GPIOTE_IRQn, 3);
    NVIC_EnableIRQ(GPIOTE_IRQn);
}

void capture_pps_enable(void)
{
    /* Rising edge = UTC second (ATGM336H), polled like DIO1; pull-down: no GPS, no edges. */
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

uint32_t capture_now32(void)
{
    NRF_TIMER1->TASKS_CAPTURE[CAP_CC_NOW] = 1;
    return NRF_TIMER1->CC[CAP_CC_NOW];
}

uint64_t capture_now64(void)
{
    uint32_t low = capture_now32();
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
    int result = s_loss_unknown ? CAPTURE_LOSS_UNKNOWN : CAPTURE_LOSS_KNOWN;
    s_loss_pending = s_loss_unknown = 0;
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return result;
}

int capture_sensor_checkpoint(uint64_t *tick, uint32_t *seq)
{
    NVIC_DisableIRQ(GPIOTE_IRQn);
    uint32_t before = sensor_count();
    *tick = capture_now64();
    uint32_t after = sensor_count();
    int empty = before == after && after == s_sensor_seq && s_head == s_tail &&
                !s_loss_pending && !NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS];
    *seq = s_sensor_seq;
    NVIC_EnableIRQ(GPIOTE_IRQn);
    return empty;
}

uint16_t capture_sensor_overflow(void)
{
    return s_overflow;
}

/* Up to 58 k strobes/s: no ISR, the main loop samples the latest CC and the count. */
void capture_strobe_enable(void)
{
    sensor_edge_hw();
}

void capture_strobe_reset(void)
{
    NRF_TIMER3->TASKS_CLEAR = 1;
    NRF_GPIOTE->EVENTS_IN[CAP_GPIOTE_SENS] = 0;
}

uint32_t capture_strobe_count(void)
{
    return sensor_count();
}

int capture_strobe_sample(uint32_t *count, uint64_t *tick)
{
    uint32_t before = sensor_count();
    uint32_t low = NRF_TIMER1->CC[CAP_CC_SENS];
    uint32_t after = sensor_count();
    if (before != after || after == 0u) { return 0; }
    *count = after;
    *tick = widen(low);
    return 1;
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
