# Photo Finish Camera

Optional line-scan photo finish camera system with car number recognition.

## Usage

`Photo Sprint` and `Photo Laps` time with a line-scan camera at the finish line.

1. Under `Settings` → `Camera Settings` → `Camera Pi`, enter the Pi's [tailnet](https://tailscale.com/docs/concepts/tailnet) name and click `Connect`.
   * The computer must be on the same tailnet. Allow the browser's local network access prompt.
2. Aim and focus the camera with `Live View`, then set `Capture`, `Detection` and `Clips`.
   * Set `Travel Direction` to the way the cars move as seen from the camera.
   * Put the detection band on the asphalt the cars cross.
   * Keep the track clear for a moment after connecting or applying settings.
3. Select `Photo Sprint` or `Photo Laps` in `Timing` and click `Start`.
   * `Photo Sprint`: the sensor mapped to `Start` times the start, the camera the finish.
   * `Photo Laps`: every car that crosses is timed by the number on its sticker (one to three digits).
4. Click a crossing to open its photo finish image. Click the image to move the front line.

## Do It Yourself!

<details>
<summary>Bill of materials</summary>

| Qty | Part |
| ---: | --- |
| 1x | Raspberry Pi 5 (4 GB+) |
| 1x | JHEL0201GM GigE line-scan camera (2048 px, 58 kHz) |
| 1x | 25 mm C-mount lens |
| 1x | fsk-timekeeper master |

</details>

1. Wire the camera to the master's `J1` and its own 12 V supply.

   | Camera 8-pin | Signal | Connect to |
   | --- | --- | --- |
   | 1 GND | Ground | Master `J1` Pin 4 (GND) and the 12 V supply's ground |
   | 2 Power | 12 V | The 12 V supply |
   | 6 Line 2+ | Line trigger | Master `J1` Pin 2 (TRIG) |
   | 8 Line 1 | Strobe | Master `J1` Pin 3 (SENSE) |

2. Connect the master to the Pi's USB port.
3. Connect the camera to the Pi's Ethernet port.
4. Run following at the Pi. Raspberry Pi OS Lite (64-bit) is highly recommended.

   ```sh
   curl -fsSL https://github.com/luftaquila/fsk-timekeeper/releases/latest/download/install-photofinish.sh | sudo bash
   ```

   * On the first run, log in to your tailnet with the link it prints.
   * Enable MagicDNS and HTTPS certificates in your tailnet.

To update, manually run `sudo fsk-timekeeper-update latest`.
