# Formula Student Korea Timekeeper

Wireless LoRa timing instrument for Formula Student Korea dynamic events.

* One USB master, up to five battery-powered sensors (two in practice), one channel: 922.9 MHz by default (KR920)
* Two modes: start → finish, laps (optional auto-stop)
* Console: one HTML file, Chrome / Edge (Web Serial)
  * https://luftaquila.github.io/fsk-timekeeper/
  * `fsk-timekeeper-console.html` from the [latest release](https://github.com/luftaquila/fsk-timekeeper/releases/latest)

## Usage notes

* The master is the board plugged into the console PC, and it must stay plugged in. Unplugged, it stops all radio activity; plugged back in, it starts a new session and any open run ends.
* Radio settings (frequency, output power, listen-before-talk) are stored on each board: set them in the console (Settings → Radio) on every master and sensor alike. A board on another frequency never hears the others.
  * Apply succeeds only after storage and radio startup. `cfgapply` / `cfgpending` mean the settings are stored but application failed / awaits initialization; the board retries automatically. Stored values alone do not confirm a working radio.
* The Ra-01SH uses a passive crystal (TCXO voltage disabled). Radio startup verifies XOSC standby, device errors and RX mode before accepting settings; radio oscillator / PLL errors are distinct from the MCU HFXO clock.
* Radio protocol 14 / USB protocol 5: update the console and the firmware of every board together. The console refuses a master that reports another protocol version and says why.
* Radio faults clear only after their corresponding operation succeeds; RX start and packet-read failures recover independently. Reinitializations belong in Device log, with per-boot attempt/failure counts and the latest reason/result. A missing master beacon alone is a link outage, not a hardware error.
  * Sensor history uses the existing checkpoint's 17-byte union: six cumulative failure counts and the recovery summary. Uplinks stay 94 bytes and slots stay 110 ms; no additional radio packets. Checkpoints follow queued measurements (normally every 5 s when idle), so history delivery can be delayed under load.
  * History is frozen on checkpoint creation, retransmitted through the existing ACK path, and stored with the checkpoint before host ACK. Counts saturate at 65535; timestamps are report times, not reconstructed event times. Counters live in RAM per boot; power loss before delivery can lose history. Existing browser storage-failure warnings and memory-only fallback also apply.
* USB reports start only when a console opens the serial port and asserts DTR; the browser console does this automatically. Cable connection alone does not accumulate USB reports or TX drops. Closing the port discards pending control replies, but unacknowledged measurement events stay queued for reconnection.
* Firmware update: Settings → Firmware Update → select `fsk-timekeeper-firmware.zip` or its inner `fsk-timekeeper-dfu.zip` → check the version → Update. Select the board when prompted; a normal board resets before the bootloader chooser opens. If the browser requires another click, use Select Bootloader. A new or unresponsive board may need a double-tap on RST.
  * Both ZIPs contain `fsk-timekeeper-metadata.json`. Packages without this metadata are rejected. The browser checks product, application region, protocol compatibility, embedded build identity, file sizes, SHA-256 and the DFU CRC before writing. Hashes detect corruption; they do not authenticate the publisher.
  * Works offline in the bundled HTML and in the hosted console. No firmware download service is used. After transfer, use Reconnect to select the application port; completion requires reading back the expected running version and protocols (and chip ID when known). A completed transfer without reconnection is shown as unverified.
  * Reinstall and downgrade are explicitly labeled. Measurement and device configuration are blocked during the update. Keep the board connected while writing; after a failed transfer, reset into bootloader and retry. Only the application region is programmed; radio settings and the fleet-key page are excluded.
* History → Export JSON saves **all modes**, including unfinished run snapshots, exact timing values, evidence, PPS and device logs. Export Excel follows the selected mode filter.
  * Import JSON beside Export JSON merges the recovery file. Identical records are skipped; conflicting records cancel the entire import. Stop measurement and finish any firmware update first. Imported unfinished runs remain stopped.
  * Verified results save automatically. PPS finalization continues across Clear, the next run and reload; without usable PPS it freezes at nominal 16 MHz. JSON and Excel export wait for any existing finalization deadline (up to 2 seconds).
  * Active runs use IndexedDB. Storage failures use the existing storage banner and non-durable record marking; export JSON before closing the page when storage fails.

## Deployment and recovery

1. Export JSON from the console before updating. Keep the matching firmware ZIP and console HTML from the current release.
2. Build and test the target revision. A `v*` tag runs firmware, PCB, housing and console workflows; all must succeed before the release publishes.
3. The release publishes firmware, standalone HTML and `fsk-timekeeper-web.zip` from the same revision. Pages deployment starts only after publication and verifies the web archive's `build-sha.txt` against the release tag.
4. Update every board with the release firmware, then use its console. Verify device identity/protocols, connection, a measured result and JSON export/import before operation.
5. To retry Pages deployment or roll back, run **Build web application** with a published `release-tag` that contains the web ZIP. It deploys that archive without rebuilding. Restore matching board firmware if needed; use Import JSON to restore records. Import never clears existing data; a conflicting record leaves storage unchanged.
   * Earlier consoles without recovery import or support for database version 3 need a separate browser profile and their matching standalone HTML. Keep the current profile and recovery JSON for restoration with a compatible console.

> [!NOTE]
> Assembly, usage and troubleshooting guides will follow.

## Development

<details>
<summary>click to expand</summary>

* Firmware
  * Requires: `arm-none-eabi-gcc` (with newlib-nano), `make`, `python3`
  * Build: `make -C device/firmware`
  * Test: `make -C device/firmware test`
* Console
  * Requires: Node.js 22
  * Build: `cd console && npm ci && npm run build:single`
  * Dev: `npm run dev` · Test: `npm test`
* Housings
  * Requires: OpenSCAD
  * Build: `make -C device/3d`
* Hardware
  * Requires: KiCad 10
  * Open: `device/hardware/fsk-timekeeper.kicad_pro`

</details>

## LICENSE

For non-commercial use only:

```
"THE BEERWARE LICENSE" (Revision 42):
LUFT-AQUILA wrote this project. As long as you retain this notice,
you can do whatever you want with this stuff. If we meet someday,
and you think this stuff is worth it, you can buy me a beer in return.
```

이 저장소의 모든 내용물은 비상업적 용도에 한해 얼마든지 자유롭게 사용할 수 있습니다.\
이 프로젝트가 마음에 든다면, 언젠가 우리가 만나게 되었을 때 맥주 한 잔 사 주세요.
