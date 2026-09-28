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
static NRF_GPIO_Type test_gpio[2];
static NRF_UARTE_Type test_uart;
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
#define __DMB() ((void)0)
static inline void NVIC_ClearPendingIRQ(unsigned irq) { (void)irq; }
static inline void NVIC_SetPriority(unsigned irq, unsigned priority) { (void)irq; (void)priority; }
static inline void NVIC_EnableIRQ(unsigned irq) { (void)irq; }
#endif
