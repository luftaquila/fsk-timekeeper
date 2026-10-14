/* One-page flash records kept outside the application image (keystore.c,
 * radio_cfg.c): App DFU rewrites only the pages in the .uf2, so they survive
 * firmware updates. */
#ifndef NVMC_H
#define NVMC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Erase the 4 KiB page at page_addr and write len bytes of data at its start,
 * word by word (data word-aligned, len a multiple of 4). Returns 0 on success,
 * -1 when the controller never reported ready; the caller verifies the readback.
 * Blocking: the erase halts the CPU for ~90 ms, so only one-shot provisioning
 * paths call it. */
int nvmc_page_write(uint32_t page_addr, const void *data, unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* NVMC_H */
