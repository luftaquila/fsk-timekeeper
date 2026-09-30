/* Memory layout for an application booted by the stock nice!nano Adafruit nRF52
 * UF2 bootloader (S140 v6.1.1 — see INFO_UF2.TXT on the board).
 *
 *   0x00000000  MBR (Nordic master boot record)         — untouched
 *   0x00001000  S140 v6.1.1 SoftDevice                   — present, NOT used
 *   0x00026000  application (this image)                 — FLASH below
 *   0x000F3000  fleet-key page (keystore.c)              — outside FLASH, survives DFU
 *   0x000F4000  Adafruit bootloader + MBR params + settings — untouched
 *
 * The bootloader only flashes UF2 blocks at >= 0x26000 (the S140 v6 app base).
 * The SoftDevice stays in flash but is never enabled, so it reserves no
 * peripherals at runtime; main() sets SCB->VTOR = 0x26000.
 *
 * RAM: across a system reset the bootloader uses 0x20007F7C.. (its double-reset
 * word, no-init block, data and stack) and the SoftDevice init low RAM, so the
 * fault record that must survive a reboot (.retained, fault.c) sits at a fixed
 * address between them. nrf_common.ld ASSERTs on region overflow.
 */
SEARCH_DIR(.)
GROUP(-lgcc -lc)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x00026000, LENGTH = 0x000CD000
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00040000
}

INCLUDE "nrf_common.ld"

SECTIONS
{
  .retained 0x20007E00 (NOLOAD) :
  {
    KEEP(*(.retained))
  }
}

ASSERT(SIZEOF(.retained) <= 0x17C, "retained block overlaps the bootloader RAM")
ASSERT(__HeapLimit <= 0x20007E00, "data + heap overlap the retained block")
