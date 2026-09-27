/* nrfx integration glue for bare-metal (no RTOS, no SoftDevice).
 *
 * Required by the nrfx HAL/driver headers that TinyUSB's nRF port pulls in
 * (nrfx_clock.h -> nrfx.h). Implemented against CMSIS (NVIC, PRIMASK) + GCC
 * atomic builtins. Only nrfx_power is actually used (USB power events).
 */
#ifndef NRFX_GLUE_H__
#define NRFX_GLUE_H__

#include <stdbool.h>
#include "nrf.h" /* CMSIS core: NVIC_*, __get_PRIMASK, __disable_irq, __NOP */

#ifdef __cplusplus
extern "C" {
#endif

#define NRFX_ASSERT(expression) ((void)(expression))

#ifdef __cplusplus
#define NRFX_STATIC_ASSERT(expression) static_assert(expression, "")
#else
#define NRFX_STATIC_ASSERT(expression) _Static_assert(expression, "")
#endif

#define NRFX_IRQ_PRIORITY_SET(irq_number, priority) NVIC_SetPriority(irq_number, priority)
#define NRFX_IRQ_ENABLE(irq_number)                 NVIC_EnableIRQ(irq_number)
#define NRFX_IRQ_IS_ENABLED(irq_number)             (NVIC_GetEnableIRQ(irq_number) != 0)
#define NRFX_IRQ_DISABLE(irq_number)                NVIC_DisableIRQ(irq_number)
#define NRFX_IRQ_PENDING_SET(irq_number)            NVIC_SetPendingIRQ(irq_number)
#define NRFX_IRQ_PENDING_CLEAR(irq_number)          NVIC_ClearPendingIRQ(irq_number)
#define NRFX_IRQ_IS_PENDING(irq_number)             (NVIC_GetPendingIRQ(irq_number) != 0)

#define NRFX_CRITICAL_SECTION_ENTER() \
    {                                 \
        uint32_t __nrfx_primask = __get_PRIMASK(); \
        __disable_irq();
#define NRFX_CRITICAL_SECTION_EXIT()  \
        __set_PRIMASK(__nrfx_primask); \
    }

#define NRFX_COREDEP_DELAY_DWT_BASED 0

static inline void nrfx_glue_delay_us(uint32_t us)
{
    /* ~64 cycles/us at the 64 MHz core; ~4 cycles/iteration -> us*16. Coarse,
     * adequate for the short waits nrfx_power performs. */
    volatile uint32_t n = us * 16u;
    while (n--) {
        __NOP();
    }
}
#define NRFX_DELAY_US(us_time) nrfx_glue_delay_us(us_time)

#define nrfx_atomic_t uint32_t

#define NRFX_ATOMIC_FETCH_STORE(p_data, value) __atomic_exchange_n((p_data), (value), __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_OR(p_data, value)    __atomic_fetch_or((p_data), (value), __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_AND(p_data, value)   __atomic_fetch_and((p_data), (value), __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_XOR(p_data, value)   __atomic_fetch_xor((p_data), (value), __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_ADD(p_data, value)   __atomic_fetch_add((p_data), (value), __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_SUB(p_data, value)   __atomic_fetch_sub((p_data), (value), __ATOMIC_SEQ_CST)

static inline uint32_t nrfx_glue_atomic_cas(uint32_t volatile *p, uint32_t expected, uint32_t desired)
{
    __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
#define NRFX_ATOMIC_CAS(p_data, old_value, new_value) nrfx_glue_atomic_cas((p_data), (old_value), (new_value))

#define NRFX_CUSTOM_ERROR_CODES 0
#define NRFX_EVENT_READBACK_ENABLED 1

#ifdef __cplusplus
}
#endif

#endif /* NRFX_GLUE_H__ */
