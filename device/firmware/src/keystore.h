/* Fleet PSK, not compiled in (CI images stay key-less): 'K' over USB writes it per board to the top
 * app-flash page, outside the linker's FLASH (nrf52840_app.ld), so DFU upgrades never touch it. */
#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTORE_KEY_LEN 32

/* 1 and key_out filled if a valid key is present; 0 if unprovisioned or the page is corrupt. */
int keystore_load(uint8_t key_out[KEYSTORE_KEY_LEN]);

/* Erase, rewrite, verify readback: 0 or <0. Page erase halts the CPU ~90 ms: provisioning only. */
int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTORE_H */
