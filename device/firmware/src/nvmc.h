/* One-page flash records outside the app image (keystore.c, radio_cfg.c) survive UF2 DFU. */
#ifndef NVMC_H
#define NVMC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Erase the 4 KiB page and write len bytes (word-aligned, len a multiple of 4): 0, or -1 if the
 * NVMC never got ready; caller verifies readback. Erase halts the CPU ~90 ms: provisioning only. */
int nvmc_page_write(uint32_t page_addr, const void *data, unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* NVMC_H */
