/* Fleet PSK store (DESIGN.md §2.11).
 *
 * The key is NOT compiled in. It lives in a dedicated flash page, written at
 * provisioning time over the USB-serial 'K' command (proto_usb). CI therefore
 * builds a key-less application that is safe to publish on a public repo; each
 * board is provisioned locally and the key never touches the repo/CI.
 *
 * The page sits at the very top of the application flash region, just below the
 * bootloader, and is carved out of the linker's FLASH length (nrf52840_app.ld).
 * App DFU updates rewrite only the pages contained in the .uf2 (the app image,
 * which is far below this page), so the key survives firmware updates.
 */
#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYSTORE_KEY_LEN 32

/* Load the provisioned key. Returns 1 and fills key_out if a valid key is
 * present, 0 if the board is unprovisioned or the page is corrupt. */
int keystore_load(uint8_t key_out[KEYSTORE_KEY_LEN]);

/* Erase + (re)write the key page with key[32]. Returns 0 on success, <0 on
 * failure (verifies the readback). Blocking: a flash page erase halts the CPU
 * for ~90 ms, so call it only from the one-shot provisioning path. */
int keystore_write(const uint8_t key[KEYSTORE_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* KEYSTORE_H */
