/* Actual production HAL: stalled DMA, BUSY, delay and micros wrap. */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "../src/radio_hal.cpp"
NRF_GPIO_Type test_gpio[2];
volatile NRF_SPIM_Type test_spim;
static uint32_t now_us, complete_at;
extern "C" uint32_t board_micros(void) {
    now_us++;
    if (complete_at && (int32_t)(now_us - complete_at) >= 0) test_spim.EVENTS_END = 1;
    return now_us;
}
extern "C" uint32_t board_millis(void) { return board_micros() / 1000u; }
extern "C" void el_note(el_code_t) {}
extern "C" void fault_hang(uint32_t, uint32_t) { abort(); }
int main(void) {
    NrfHal h(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_BUSY);
    h.init(); uint8_t out[128] = {0}, in[128] = {0};
    h.limit(now_us + 500); uint32_t start = now_us;
    h.spiTransfer(out, sizeof(out), in);
    assert(now_us - start <= 505 && h.deadlineHit() && h.takeSpiTimeout());
    assert(test_spim.TASKS_STOP && test_spim.ENABLE == 0 && in[0] == 0xff && in[127] == 0xff);
    /* Expired access cannot toggle NSS or spin forever on BUSY. */
    test_gpio[0].IN = UINT32_MAX;
    assert(h.digitalRead(PIN_LORA_BUSY) == 0);
    uint32_t before = test_gpio[0].OUTSET;
    h.digitalWrite(PIN_LORA_NSS, 1); assert(test_gpio[0].OUTSET == before);
    h.unlimit(); h.limit(now_us + 1000); complete_at = now_us + 100;
    h.spiTransfer(out, sizeof(out), in);
    assert(!h.takeSpiTimeout() && !h.deadlineHit() && test_spim.ENABLE == SPIM_ENABLE_ENABLE_Enabled);
    complete_at = 0; h.limit(now_us + 50); start = now_us;
    h.delay(500); assert(now_us - start <= 53 && h.deadlineHit());
    now_us = UINT32_MAX - 30; h.limit(now_us + 50); start = now_us;
    h.delayMicroseconds(500); assert((uint32_t)(now_us - start) <= 53 && h.deadlineHit());
    puts("PASS production_hal_deadlines_dma_busy_wrap");
}
