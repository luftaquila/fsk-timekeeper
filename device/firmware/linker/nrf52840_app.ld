/* Memory layout under the stock nice!nano Adafruit nRF52 UF2 bootloader (S140 v6.1.1).
 *
 *   0x00000000  MBR                                    untouched
 *   0x00001000  S140 v6.1.1 SoftDevice                 present, never enabled
 *   0x00026000  application (this image)               FLASH below; board_init() sets VTOR
 *   0x000F2000  radio settings page (radio_cfg.c)      outside FLASH, survives DFU
 *   0x000F3000  fleet-key page (keystore.c)            outside FLASH, survives DFU
 *   0x000F4000  bootloader + MBR params + settings     untouched
 *
 * The bootloader flashes UF2 blocks only at >= 0x26000. RAM: .retained (fault.c) sits at a fixed
 * address between the SoftDevice-init low RAM and the bootloader's 0x20007F7C.. area, so it
 * survives a reset; nrf_common.ld ASSERTs on region overflow.
 */
SEARCH_DIR(.)
GROUP(-lgcc -lc)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x00026000, LENGTH = 0x000CC000
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
