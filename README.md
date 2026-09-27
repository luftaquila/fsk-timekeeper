# Formula Student Korea Timekeeper

Wireless LoRa timing instrument for Formula Student Korea dynamic events.

* One USB master, up to six battery-powered sensors, one 921.3 MHz channel (KR920)
* Two modes: start → finish, laps (optional auto-stop)
* Console: one HTML file, Chrome / Edge (Web Serial)
  * https://luftaquila.github.io/fsk-timekeeper/
  * `fsk-timekeeper-console.html` from the [latest release](https://github.com/luftaquila/fsk-timekeeper/releases/latest)

> [!NOTE]
> Assembly, usage and troubleshooting guides will follow.

## Development

<details>
<summary>click to expand</summary>

* Firmware
  * Requires: `arm-none-eabi-gcc` (with newlib-nano), `make`, `python3`
  * Build: `make -C device/firmware`
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
