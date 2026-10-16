/* Host-only register surface used by gps.c and gpio.h; no hardware emulation. */
#ifndef TEST_NRF_H
#define TEST_NRF_H
#include <stdint.h>

typedef struct {
    uint32_t DIRSET, OUTSET, OUTCLR, OUT, IN, PIN_CNF[32];
} NRF_GPIO_Type;
typedef struct {
    uint32_t EVENTS_ENDRX, EVENTS_ENDTX, TASKS_STARTTX, TASKS_STARTRX;
    uint32_t ENABLE, BAUDRATE, CONFIG, SHORTS, INTENSET;
    struct { uint32_t RXD, TXD, RTS, CTS; } PSEL;
    struct { uint32_t PTR, MAXCNT; } RXD, TXD;
} NRF_UARTE_Type;
/* Each test defines only the register banks it uses. */
extern NRF_GPIO_Type test_gpio[2];
extern NRF_UARTE_Type test_uart;
#define NRF_P0 (&test_gpio[0])
#define NRF_P1 (&test_gpio[1])
#define NRF_UARTE0 (&test_uart)
#define GPIO_PIN_CNF_DIR_Input 0u
#define GPIO_PIN_CNF_DIR_Pos 0u
#define GPIO_PIN_CNF_INPUT_Connect 0u
#define GPIO_PIN_CNF_INPUT_Pos 1u
#define GPIO_PIN_CNF_PULL_Pullup 3u
#define GPIO_PIN_CNF_PULL_Pulldown 1u
#define GPIO_PIN_CNF_PULL_Disabled 0u
#define GPIO_PIN_CNF_PULL_Pos 2u
#define UARTE_ENABLE_ENABLE_Disabled 0u
#define UARTE_ENABLE_ENABLE_Enabled 8u
#define UARTE_BAUDRATE_BAUDRATE_Baud9600 0x00275000u
#define UARTE_CONFIG_HWFC_Disabled 0u
#define UARTE_CONFIG_HWFC_Pos 0u
#define UARTE_CONFIG_PARITY_Excluded 0u
#define UARTE_CONFIG_PARITY_Pos 1u
#define UARTE_SHORTS_ENDRX_STARTRX_Msk 32u
#define UARTE_INTENSET_ENDRX_Msk 16u
#define UART0_UARTE0_IRQn 2u
typedef struct {
    uint32_t TASKS_STOP, TASKS_START, TASKS_CLEAR, TASKS_COUNT, TASKS_CAPTURE[6];
    uint32_t MODE, BITMODE, PRESCALER, CC[6];
} NRF_TIMER_Type;
typedef struct { uint32_t CONFIG[8], EVENTS_IN[8], INTENSET; } NRF_GPIOTE_Type;
typedef struct {
    struct { uint32_t EEP, TEP; } CH[32];
    struct { uint32_t TEP; } FORK[32];
    uint32_t CHENSET;
} NRF_PPI_Type;
extern NRF_TIMER_Type test_timer1, test_timer3;
extern NRF_GPIOTE_Type test_gpiote;
extern NRF_PPI_Type test_ppi;
#define NRF_TIMER1 (&test_timer1)
#define NRF_TIMER3 (&test_timer3)
#define NRF_GPIOTE (&test_gpiote)
#define NRF_PPI (&test_ppi)
#define TIMER_MODE_MODE_Timer 0u
#define TIMER_MODE_MODE_Counter 1u
#define TIMER_BITMODE_BITMODE_32Bit 3u
#define GPIOTE_CONFIG_MODE_Event 1u
#define GPIOTE_CONFIG_MODE_Pos 0u
#define GPIOTE_CONFIG_PSEL_Pos 8u
#define GPIOTE_CONFIG_PORT_Pos 13u
#define GPIOTE_CONFIG_POLARITY_Pos 16u
#define GPIOTE_CONFIG_POLARITY_LoToHi 1u
#define GPIOTE_CONFIG_POLARITY_HiToLo 2u
#define GPIOTE_INTENSET_IN0_Pos 0u
#define GPIOTE_IRQn 6u
typedef struct {
    uint32_t ENABLE, FREQUENCY, CONFIG, EVENTS_END, TASKS_START, TASKS_STOP;
    struct { uint32_t SCK, MISO, MOSI; } PSEL;
    struct { uintptr_t PTR; uint32_t MAXCNT; } RXD, TXD;
} NRF_SPIM_Type;
extern volatile NRF_SPIM_Type test_spim;
#define NRF_SPIM0 (&test_spim)
#define SPIM_ENABLE_ENABLE_Disabled 0u
#define SPIM_ENABLE_ENABLE_Enabled 7u
#define SPIM_ENABLE_ENABLE_Pos 0u
#define SPIM_FREQUENCY_FREQUENCY_M1 0x10000000u
#define __DMB() ((void)0)
static inline void NVIC_DisableIRQ(unsigned irq) { (void)irq; }
static inline void NVIC_SystemReset(void) { }
static inline void NVIC_ClearPendingIRQ(unsigned irq) { (void)irq; }
static inline void NVIC_SetPriority(unsigned irq, unsigned priority) { (void)irq; (void)priority; }
static inline void NVIC_EnableIRQ(unsigned irq) { (void)irq; }
#endif
