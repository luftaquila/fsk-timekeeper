# Formula Student Korea Timekeeper

Wireless timing system for Formula Student Korea dynamic events.

## Features

* LoRa wireless communication with one USB master and up to five battery-powered sensors
* Start → Finish and lap timing with optional lap-count auto-stop
* Web-based console with offline support
* Excel export and JSON backup / restore
* Firmware updates through the browser

## Sensor connector

* Sensor: Autonics **BA2M-DDT**
* Board: Molex Mini-Fit Jr. 5569-04A2 · Cable: Molex 5557-04R (39012040)
* Pinout: `1: +12V` · `2: NC` · `3: SENSE` · `4: GND`

<img src=".github/assets/sensor-pinout.png" alt="J1 sensor connector pinout" width="550">

Board connector viewed from the mating side, latch at the top.

![BA2M-DDT sensor cable wiring](.github/assets/wire.png)

Cable-side connector shown above. Wire colors follow the [BA2M-DDT datasheet, p. 2](https://autonics.se/wp-content/uploads/2018/02/BA_cat_eng_161109.pdf). Leave pin 2 unconnected.

## Usage

Open the [online console](https://luftaquila.github.io/fsk-timekeeper/) in Chrome or Edge.
For offline use, download `fsk-timekeeper-console.html` from the [latest release](https://github.com/luftaquila/fsk-timekeeper/releases/latest).

### 1. Set up devices

1. Connect each board to the PC over USB and click `Connect` in Settings.
2. Under Device Settings, apply the same Radio settings and Radio Key to every board. The default frequency is 922.9 MHz.
   * Generate a key once, then copy and apply it to the other boards.
3. Leave the master connected to the PC and power the sensors from their batteries.
4. In Settings → Sensors, assign each sensor to `Start` or `Finish`. Lap timing uses the `Start` sensor.

> [!IMPORTANT]
> Use firmware and console from the same release on all devices. Keep the master connected during measurement; reconnecting it ends the current run.

### 2. Measure

1. Select `Start → Finish` or `Laps` on the Timing page.
2. Click `Start` to arm the timer. Timing begins when the vehicle crosses the start sensor.
3. For Start → Finish, cross the finish sensor to complete the run. For Laps, each subsequent crossing records a lap; set a lap target to stop automatically, or click `Stop`.

### 3. Save results

Results are saved in the browser and listed in History.

* `Export Excel`: Export results for the selected mode.
* `Export JSON`: Back up all modes, including unfinished runs and device logs.
* `Import JSON`: Restore a backup. Identical records are skipped; conflicting records cancel the import.

> [!IMPORTANT]
> Browser storage is not a backup. Export JSON after each session and before updating. If a storage error appears, export before closing the page.

### 4. Update firmware

1. Download `fsk-timekeeper-firmware.zip` from the [latest release](https://github.com/luftaquila/fsk-timekeeper/releases/latest).
2. Stop measurement and connect the board over USB.
3. Open Settings → Device Settings → Firmware Update, select the ZIP and click `Update`.
4. Select the bootloader port when prompted. After transfer, click `Reconnect` to verify the running version.
5. Repeat for every board and use the console from the same release.

If the bootloader is not detected, double-tap the board's RST button and retry. Keep the board connected until the update completes.

## Development

<details>
<summary>click to expand</summary>

Run commands from the repository root unless noted otherwise.

* Firmware: Arm GNU Toolchain (`arm-none-eabi-gcc`, with newlib-nano), Make, Python 3

  ```sh
  make -C device/firmware
  make -C device/firmware test
  ```

* Console: Node.js 22

  ```sh
  cd console
  npm ci
  npm run dev           # development server
  npm run build:single  # standalone HTML at dist-single/index.html
  npm test
  ```

* PCB: Open [fsk-timekeeper.kicad_pro](device/hardware/fsk-timekeeper.kicad_pro) with KiCad 10.
* Housing: Install OpenSCAD and run `make -C device/3d` to export STL files.

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
