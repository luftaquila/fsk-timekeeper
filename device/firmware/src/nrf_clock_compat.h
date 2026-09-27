#ifndef NRF_CLOCK_COMPAT_H
#define NRF_CLOCK_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

/* TinyUSB 0.20 uses the pre-nrfx-4 name; usb.c supplies this adapter. */
bool nrf_clock_hf_is_running(void *p_reg, uint32_t hfclk_src);

#endif
