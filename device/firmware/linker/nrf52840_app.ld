/* Memory layout for an application booted by the stock nice!nano Adafruit nRF52
 * UF2 bootloader (S140 v6.1.1 — see INFO_UF2.TXT on the board).
 *
 *   0x00000000  MBR (Nordic master boot record)         — untouched
 *   0x00001000  S140 v6.1.1 SoftDevice                   — present, NOT used
 *   0x00026000  application (this image)                 — FLASH below
 *   0x000F4000  Adafruit bootloader + MBR params + settings — untouched
 *
 * The bootloader PROTECTS the SoftDevice region (0x1000–0x26000) and only
 * flashes UF2 blocks targeting the user-app region (>= 0x26000). An app linked
 * at 0x1000 has every block silently rejected — the file copies onto the drive
 * but never flashes. So we link at the S140 v6 app base (0x26000). We keep the
 * SoftDevice in flash but never call sd_softdevice_enable(), so it reserves no
 * peripherals at runtime (TIMER0/RTC0/PPI stay free for the capture design);
 * boot chain is MBR -> S140 -> app@0x26000. main() sets SCB->VTOR = 0x26000.
 *
 * LENGTH stops one page below the bootloader region: 0xF3000..0xF4000 is the
 * fleet-key store (keystore.c), carved out of FLASH so the app never occupies
 * it and the app .uf2 never rewrites it (the key survives DFU). nrf_common.ld
 * ASSERTs on overflow.
 */
SEARCH_DIR(.)
GROUP(-lgcc -lc)

MEMORY
{
  /* 0x26000..0xF3000 app; 0xF3000..0xF4000 reserved for keystore.c (DESIGN §2.11) */
  FLASH (rx) : ORIGIN = 0x00026000, LENGTH = 0x000CD000
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00040000
}

INCLUDE "nrf_common.ld"
