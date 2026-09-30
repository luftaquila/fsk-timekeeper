#include "fault.h"

#include <string.h>

#include "config.h"
#include "proto_usb.h"

#define FAULT_MAGIC 0xFA17B007u

#define RR_RESETPIN (1UL << 0)
#define RR_DOG      (1UL << 1)
#define RR_SREQ     (1UL << 2)
#define RR_LOCKUP   (1UL << 3)
#define RR_OFF      (1UL << 16)
#define RR_LPCOMP   (1UL << 17)
#define RR_DIF      (1UL << 18)
#define RR_NFC      (1UL << 19)
#define RR_VBUS     (1UL << 20)

static uint32_t rec_check(const fault_rec_t *r)
{
    return r->magic ^ r->count ^ r->pending ^ r->pc ^ r->lr ^ r->cfsr ^ r->cause ^ 0x5A5AA5A5u;
}

static int rec_valid(const fault_rec_t *r)
{
    return r->magic == FAULT_MAGIC && r->check == rec_check(r);
}

static void rec_reset(fault_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->magic = FAULT_MAGIC;
    r->check = rec_check(r);
}

uint8_t fault_eval_boot(fault_rec_t *r, uint32_t rr, int *report)
{
    uint8_t reason = 0;
    if (rr & RR_RESETPIN) { reason |= RESET_PIN; }
    if (rr & RR_DOG)      { reason |= RESET_DOG; }
    if (rr & RR_SREQ)     { reason |= RESET_SREQ; }
    if (rr & RR_LOCKUP)   { reason |= RESET_LOCKUP; }
    if (rr & RR_OFF)      { reason |= RESET_OFF; }
    if (rr & RR_VBUS)     { reason |= RESET_VBUS; }
    if (rr & (RR_LPCOMP | RR_DIF | RR_NFC)) { reason |= RESET_OTHER; }

    *report = 0;
    if (!rec_valid(r)) { /* power-on: RAM content is undefined */
        rec_reset(r);
        return reason;
    }
    if (r->pending) {
        r->pending = 0;
        reason |= RESET_FAULT;
        *report = 1;
    }
    if (rr & RR_LOCKUP) {
        /* a fault inside fault handling: counts like a fault reboot */
        if (r->count != UINT32_MAX) { r->count++; }
    } else if (!(rr & RR_SREQ)) {
        r->count = 0; /* pin reset / power cycle: a fresh start */
    }
    r->check = rec_check(r);
    return reason;
}

int fault_note(fault_rec_t *r, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t cause)
{
    if (!rec_valid(r)) { rec_reset(r); }
    r->pc = pc;
    r->lr = lr;
    r->cfsr = cfsr;
    r->cause = cause;
    r->pending = 1;
    if (r->count != UINT32_MAX) { r->count++; }
    r->check = rec_check(r);
    return r->count > FAULT_REBOOT_MAX;
}

#ifndef FAULT_HOST_TEST

#include "gpio.h"
#include "nrf.h"

/* Fixed RAM block outside what the bootloader (0x20007F7C..) and the
 * SoftDevice/MBR init (low RAM) touch across a system reset (nrf52840_app.ld). */
__attribute__((section(".retained"))) static fault_rec_t s_rec;

static uint8_t s_reason;
static int s_report;
static fault_rec_t s_seen;
static int s_stable;

static void halt_blink(void)
{
    __disable_irq();
    gpio_cfg_output(PIN_LED_STATUS);
    for (;;) {
        gpio_toggle(PIN_LED_STATUS);
        for (volatile uint32_t i = 0; i < 400000u; i++) { }
    }
}

void fault_boot(void)
{
    /* Without these enables every fault escalates to HardFault and the record
     * always says "hard" (CFSR still holds the cause). */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
    uint32_t rr = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = rr; /* bits clear by writing 1; the bootloader leaves them */
    int report;
    s_seen = s_rec;
    s_reason = fault_eval_boot(&s_rec, rr, &report);
    s_report = report;
    if (s_rec.count > FAULT_REBOOT_MAX) { halt_blink(); }
}

uint8_t fault_reset_reason(void)
{
    return s_reason;
}

int fault_take_report(uint32_t *pc, uint32_t *lr, const char **cause, uint32_t *cfsr)
{
    static const char *const k_cause[] = { "?", "hard", "mem", "bus", "usage", "irq" };
    if (!s_report) { return 0; }
    s_report = 0;
    *pc = s_seen.pc;
    *lr = s_seen.lr;
    *cfsr = s_seen.cfsr;
    *cause = s_seen.cause < sizeof(k_cause) / sizeof(k_cause[0]) ? k_cause[s_seen.cause] : "?";
    return 1;
}

void fault_service(uint32_t uptime_ms)
{
    if (s_stable || uptime_ms < FAULT_STABLE_MS) { return; }
    s_stable = 1;
    s_rec.count = 0;
    s_rec.check = rec_check(&s_rec);
}

/* frame = the exception stack frame (r0 r1 r2 r3 r12 lr pc xpsr). For an
 * unexpected interrupt the cfsr slot carries IPSR (the vector number). */
__attribute__((used, noreturn)) void fault_capture(const uint32_t *frame, uint32_t cause)
{
    uint32_t detail = cause == FAULT_CAUSE_IRQ ? __get_IPSR() : SCB->CFSR;
    if (fault_note(&s_rec, frame[6], frame[5], detail, cause)) { halt_blink(); }
    NVIC_SystemReset();
}

#define FAULT_ENTRY(name, cause)                         \
    __attribute__((naked)) void name(void)               \
    {                                                    \
        __asm volatile("tst lr, #4      \n"              \
                       "ite eq          \n"              \
                       "mrseq r0, msp   \n"              \
                       "mrsne r0, psp   \n"              \
                       "movs r1, #" #cause "\n"          \
                       "b fault_capture \n");            \
    }

FAULT_ENTRY(HardFault_Handler, 1)
FAULT_ENTRY(MemoryManagement_Handler, 2)
FAULT_ENTRY(BusFault_Handler, 3)
FAULT_ENTRY(UsageFault_Handler, 4)
FAULT_ENTRY(fault_unexpected_irq, 5)

/* Every vector this firmware does not use (the MDK defaults spin forever). */
#define UNUSED_VECTOR(name) void name(void) __attribute__((alias("fault_unexpected_irq")));
UNUSED_VECTOR(NMI_Handler)
UNUSED_VECTOR(SVC_Handler)
UNUSED_VECTOR(DebugMon_Handler)
UNUSED_VECTOR(PendSV_Handler)
UNUSED_VECTOR(SysTick_Handler)
UNUSED_VECTOR(RADIO_IRQHandler)
UNUSED_VECTOR(SPI0_SPIM0_SPIS0_TWI0_TWIM0_TWIS0_IRQHandler)
UNUSED_VECTOR(SPI1_SPIM1_SPIS1_TWI1_TWIM1_TWIS1_IRQHandler)
UNUSED_VECTOR(NFCT_IRQHandler)
UNUSED_VECTOR(SAADC_IRQHandler)
UNUSED_VECTOR(TIMER0_IRQHandler)
UNUSED_VECTOR(TIMER1_IRQHandler)
UNUSED_VECTOR(TIMER2_IRQHandler)
UNUSED_VECTOR(RTC0_IRQHandler)
UNUSED_VECTOR(TEMP_IRQHandler)
UNUSED_VECTOR(RNG_IRQHandler)
UNUSED_VECTOR(ECB_IRQHandler)
UNUSED_VECTOR(AAR_CCM_IRQHandler)
UNUSED_VECTOR(WDT_IRQHandler)
UNUSED_VECTOR(RTC1_IRQHandler)
UNUSED_VECTOR(QDEC_IRQHandler)
UNUSED_VECTOR(COMP_LPCOMP_IRQHandler)
UNUSED_VECTOR(EGU0_SWI0_IRQHandler)
UNUSED_VECTOR(EGU1_SWI1_IRQHandler)
UNUSED_VECTOR(EGU2_SWI2_IRQHandler)
UNUSED_VECTOR(EGU3_SWI3_IRQHandler)
UNUSED_VECTOR(EGU4_SWI4_IRQHandler)
UNUSED_VECTOR(EGU5_SWI5_IRQHandler)
UNUSED_VECTOR(TIMER3_IRQHandler)
UNUSED_VECTOR(TIMER4_IRQHandler)
UNUSED_VECTOR(PWM0_IRQHandler)
UNUSED_VECTOR(PDM_IRQHandler)
UNUSED_VECTOR(MWU_IRQHandler)
UNUSED_VECTOR(PWM1_IRQHandler)
UNUSED_VECTOR(PWM2_IRQHandler)
UNUSED_VECTOR(SPI2_SPIM2_SPIS2_IRQHandler)
UNUSED_VECTOR(RTC2_IRQHandler)
UNUSED_VECTOR(I2S_IRQHandler)
UNUSED_VECTOR(FPU_IRQHandler)
UNUSED_VECTOR(UARTE1_IRQHandler)
UNUSED_VECTOR(QSPI_IRQHandler)
UNUSED_VECTOR(CRYPTOCELL_IRQHandler)
UNUSED_VECTOR(PWM3_IRQHandler)
UNUSED_VECTOR(SPIM3_IRQHandler)

#endif /* FAULT_HOST_TEST */
