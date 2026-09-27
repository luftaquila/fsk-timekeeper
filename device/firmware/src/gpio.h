/* Tiny port-aware GPIO helpers over the MDK registers.
 *
 * Stage 1 keeps GPIO on raw registers (no nrfx_config/glue): the nrfx nrf_gpio
 * HAL would pull in the whole <nrfx.h> chain for no functional gain on plain
 * I/O. nrfx proper is introduced with the radio/capture (Stage 2/3), where its
 * drivers earn their setup. Pin numbering: P0.n == n, P1.n == 32 + n (config.h).
 */
#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>
#include "nrf.h"

static inline NRF_GPIO_Type *gpio_port(uint32_t pin)
{
    return (pin < 32) ? NRF_P0 : NRF_P1;
}

static inline uint32_t gpio_bit(uint32_t pin)
{
    return 1UL << (pin & 31u);
}

static inline void gpio_cfg_output(uint32_t pin)
{
    gpio_port(pin)->DIRSET = gpio_bit(pin);
}

static inline void gpio_cfg_input_pullup(uint32_t pin)
{
    gpio_port(pin)->PIN_CNF[pin & 31u] =
        ((uint32_t)GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)   |
        ((uint32_t)GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        ((uint32_t)GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos);
}

/* Plain input, no pull — for lines the far end drives (e.g. radio BUSY/DIO1). */
static inline void gpio_cfg_input(uint32_t pin)
{
    gpio_port(pin)->PIN_CNF[pin & 31u] =
        ((uint32_t)GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)   |
        ((uint32_t)GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        ((uint32_t)GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos);
}

static inline void gpio_set(uint32_t pin)
{
    gpio_port(pin)->OUTSET = gpio_bit(pin);
}

static inline void gpio_clear(uint32_t pin)
{
    gpio_port(pin)->OUTCLR = gpio_bit(pin);
}

static inline void gpio_write(uint32_t pin, int high)
{
    if (high) {
        gpio_set(pin);
    } else {
        gpio_clear(pin);
    }
}

/* Reads the input buffer — only valid on pins configured as input. */
static inline uint32_t gpio_read(uint32_t pin)
{
    return (gpio_port(pin)->IN >> (pin & 31u)) & 1UL;
}

/* Toggle an output via its OUT register (the input buffer is disconnected on
 * output pins, so gpio_read() must not be used for this). */
static inline void gpio_toggle(uint32_t pin)
{
    NRF_GPIO_Type *port = gpio_port(pin);
    uint32_t bit = gpio_bit(pin);
    if (port->OUT & bit) {
        port->OUTCLR = bit;
    } else {
        port->OUTSET = bit;
    }
}

#endif /* GPIO_H */
