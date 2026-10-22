#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CAPTURE_HOST_TEST
#include "../src/capture.c"

NRF_GPIO_Type test_gpio[2];
NRF_TIMER_Type test_timer1, test_timer3;
NRF_GPIOTE_Type test_gpiote;
NRF_PPI_Type test_ppi;
static uint32_t count, read_no, inject_on;
int board_hfclk_xtal(void) { return 1; }

static void edge(uint32_t tick)
{
    count++;
    test_timer1.CC[CAP_CC_SENS] = tick;
    test_timer1.CC[CAP_CC_NOW] = tick + 1;
    test_gpiote.EVENTS_IN[CAP_GPIOTE_SENS] = 1;
}
uint32_t capture_test_count(void)
{
    if (++read_no == inject_on) { edge(test_timer1.CC[CAP_CC_SENS] + 10); }
    return count;
}
static void reset(void)
{
    memset(&test_timer1, 0, sizeof test_timer1);
    memset(&test_gpiote, 0, sizeof test_gpiote);
    s_head = s_tail = 0;
    s_loss_pending = s_loss_unknown = 0;
    s_sensor_seq = count = read_no = inject_on = 0;
    s_base = s_prev = s_overflow = 0;
    capture_init(); capture_sensor_enable();
    assert(test_timer3.MODE == TIMER_MODE_MODE_Counter);
    assert(test_ppi.FORK[CAP_PPI_SENS].TEP == (uint32_t)&test_timer3.TASKS_COUNT);
}
int main(void)
{
    uint64_t tick, end; uint32_t seq, last; int xtal;
    reset();
    edge(100); GPIOTE_IRQHandler();
    assert(capture_sensor_get(&tick, &seq, &xtal) && seq == 1 && tick == 100 && xtal);
    edge(200); edge(300); edge(400); GPIOTE_IRQHandler();
    assert(!capture_sensor_get(&tick, &seq, &xtal));
    assert(!capture_sensor_checkpoint(&tick, &seq));
    assert(capture_sensor_loss(&tick, &end, &seq, &last) == CAPTURE_LOSS_UNKNOWN);
    assert(seq == 2 && last == 4 && capture_sensor_overflow() == 3);
    assert(capture_sensor_checkpoint(&tick, &seq) && seq == 4);
    edge(500); GPIOTE_IRQHandler();
    assert(capture_sensor_get(&tick, &seq, &xtal) && seq == 5 && tick == 500);
    puts("PASS capture_masked_edges");

    reset(); edge(100); inject_on = 2; GPIOTE_IRQHandler();
    assert(capture_sensor_loss(&tick, &end, &seq, &last) == CAPTURE_LOSS_UNKNOWN);
    assert(seq == 1 && last == 2);
    GPIOTE_IRQHandler(); /* pending event already accounted for; never duplicate it */
    assert(!capture_sensor_get(&tick, &seq, &xtal));
    assert(capture_sensor_checkpoint(&tick, &seq));
    reset(); inject_on = 2;
    assert(!capture_sensor_checkpoint(&tick, &seq));
    GPIOTE_IRQHandler();
    assert(capture_sensor_get(&tick, &seq, &xtal) && seq == 1);
    puts("PASS capture_snapshot_races");

    reset();
    for (unsigned i = 1; i <= 70; i++) { edge(i * 100); GPIOTE_IRQHandler(); }
    for (unsigned i = 1; i <= 63; i++) {
        assert(capture_sensor_get(&tick, &seq, &xtal) && seq == i && tick == i * 100);
    }
    assert(capture_sensor_loss(&tick, &end, &seq, &last) == CAPTURE_LOSS_KNOWN);
    assert(seq == 64 && last == 70 && tick == 6400 && end == 7000);
    assert(capture_sensor_checkpoint(&tick, &seq) && seq == 70);
    reset(); count = s_sensor_seq = UINT32_MAX - 1u;
    edge(100); GPIOTE_IRQHandler(); edge(200); GPIOTE_IRQHandler();
    assert(capture_sensor_get(&tick, &seq, &xtal) && seq == UINT32_MAX);
    assert(capture_sensor_get(&tick, &seq, &xtal) && seq == 0);
    puts("PASS capture_overflow_wrap");

    /* Camera Strobe: the sensor's latch and count without the ISR; a sample needs a quiet read. */
    memset(&test_gpiote, 0, sizeof test_gpiote);
    count = read_no = inject_on = 0;
    capture_init(); capture_strobe_enable();
    assert(test_gpiote.INTENSET == 0 && test_ppi.FORK[CAP_PPI_SENS].TEP == (uint32_t)&test_timer3.TASKS_COUNT);
    uint32_t strobes;
    assert(!capture_strobe_sample(&strobes, &tick)); /* none yet */
    edge(100); edge(200);
    assert(capture_strobe_sample(&strobes, &tick) && strobes == 2 && tick == 200);
    inject_on = read_no + 2u; /* a strobe between the two count reads */
    assert(!capture_strobe_sample(&strobes, &tick));
    assert(capture_strobe_sample(&strobes, &tick) && strobes == 3 && tick == 210);
    test_timer3.TASKS_CLEAR = 0;
    capture_strobe_reset();
    assert(test_timer3.TASKS_CLEAR && !test_gpiote.EVENTS_IN[CAP_GPIOTE_SENS]);
    count = 0; /* what TASKS_CLEAR does to the counter */
    assert(capture_strobe_count() == 0 && !capture_strobe_sample(&strobes, &tick));
    puts("PASS capture_strobe");
    return 0;
}
