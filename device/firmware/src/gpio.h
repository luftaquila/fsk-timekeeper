/* MDK-register GPIO (nrfx HAL drags in <nrfx.h>); pins: P0.n == n, P1.n == 32 + n (config.h). */
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

/* An unpopulated/unpowered driver reads as a steady low. */
static inline void gpio_cfg_input_pulldown(uint32_t pin)
{
    gpio_port(pin)->PIN_CNF[pin & 31u] =
        ((uint32_t)GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)   |
        ((uint32_t)GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        ((uint32_t)GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos);
}

/* No pull: for lines the far end drives (e.g. radio BUSY/DIO1). */
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

/* Input buffer: valid only on pins configured as input. */
static inline uint32_t gpio_read(uint32_t pin)
{
    return (gpio_port(pin)->IN >> (pin & 31u)) & 1UL;
}

/* Uses OUT: output pins have their input buffer disconnected, so gpio_read() is invalid here. */
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
