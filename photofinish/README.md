# Photo Finish Camera

Optional line-scan photo finish camera system with car number recognition.

## Usage

`Photo Sprint` and `Photo Laps` time with a line-scan camera at the finish line.

1. Under `Settings` → `Camera Settings` → `Camera Pi`, enter the Pi's [tailnet](https://tailscale.com/docs/concepts/tailnet) name and click `Connect`.
   * The computer must be on the same tailnet. Allow the browser's local network access prompt.
2. Aim and focus the camera with `Live View`, then set `Capture`, `Detection` and `Clips`.
   * Set `Travel Direction` to the way the cars move as seen from the camera.
   * Put the detection band on the asphalt the cars cross.
3. Select `Photo Sprint` or `Photo Laps` in `Timing` and click `Start`.
   * Keep cars and people out of the camera's view when clicking `Start`: the camera learns the empty track then.
   * The camera runs only from `Start` to `Stop` and while `Live View` is on.
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
| 1x | [Molex 5569-04A2 (39300040)](https://www.molex.com/en-us/products/part-detail/39300040) board connector |
| 1x | [Molex 5557-04R (39012040)](https://www.molex.com/en-us/products/part-detail/39012040) cable housing |
| 3x | [Molex 5556T (39000038)](https://www.molex.com/en-us/products/part-detail/39000038) crimp terminal |

</details>

1. Solder the `5569-04A2` connector at the master's `J1` and house the master in `photofinish.stl` instead of `master.stl`.
2. Wire the camera to the master's `J1` and its own 12 V supply.

   | Camera 8-pin | Signal | Connect to |
   | --- | --- | --- |
   | 1 GND | Ground | Master `J1` Pin 4 (GND) and the 12 V supply's ground |
   | 2 Power | 12 V | The 12 V supply |
   | 7 Line 0 | Line trigger | Master `J1` Pin 2 (TRIG) |
   | 8 Line 1 | Strobe | Master `J1` Pin 3 (SENSE) |

3. Connect the master to the Pi's USB port.
4. Connect the camera to the Pi's Ethernet port.
5. Run following at the Pi. Raspberry Pi OS Lite (64-bit) is highly recommended.

   ```sh
   curl -fsSL https://github.com/luftaquila/fsk-timekeeper/releases/latest/download/install-photofinish.sh | sudo bash
   ```

   * On the first run, log in to your tailnet with the link it prints.
   * Enable MagicDNS and HTTPS certificates in your tailnet.

To update, manually run `sudo fsk-timekeeper-update latest`.
