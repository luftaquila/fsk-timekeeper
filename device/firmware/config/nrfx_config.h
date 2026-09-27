/* nrfx peripheral configuration. Only nrfx_power is used (USB power events for
 * TinyUSB); everything else stays off. Unspecified options fall back to nrfx's
 * internal defaults. */
#ifndef NRFX_CONFIG_H__
#define NRFX_CONFIG_H__

#define NRFX_POWER_ENABLED 1
#define NRFX_POWER_DEFAULT_CONFIG_IRQ_PRIORITY 7

#define NRFX_CLOCK_ENABLED 0

#endif /* NRFX_CONFIG_H__ */
