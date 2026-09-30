#!/bin/sh
# Flash the fsk-timekeeper firmware over the Adafruit nRF52 bootloader's serial DFU.
#
#   ./flash.sh [--serial <devid>] [port]
#
# Finds the board by itself. A board running the FSK-WL app is rebooted into the
# bootloader first (1200-baud touch); a board that is already in the bootloader
# (new board, or after a double-tap on RST) is flashed directly. With several
# boards plugged in, --serial picks the app board whose USB serial number is the
# given chip id (the 16 hex digits of its `I` line).
# Linux users need permission on the serial port (usually the `dialout` group).
set -eu
cd "$(dirname "$0")"

SERIAL=""
PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
    --serial) [ $# -ge 2 ] || { echo "--serial needs the board's chip id" >&2; exit 1; }
              SERIAL="$(printf %s "$2" | tr '[:lower:]' '[:upper:]')"; shift 2 ;;
    *) PORT="$1"; shift ;;
    esac
done

# Serial port of the first USB device with this vendor id (and product id and
# serial number when given), hex.
usb_port() {
    vid="$1"
    pid="${2:-}"
    want="${3:-}"
    case "$(uname)" in
    Darwin)
        ioreg -r -c IOUSBHostDevice -l 2>/dev/null | awk -v vid="$((0x$vid))" \
            -v pid="$( [ -n "$pid" ] && printf %d "0x$pid" || echo -1 )" -v want="$want" '
            /"idVendor" = /          { v = $NF }
            /"idProduct" = /         { p = $NF }
            /"USB Serial Number" = / { s = $NF; gsub(/"/, "", s); s = toupper(s) }
            /"IOCalloutDevice" = /   { if (v == vid && (pid < 0 || p == pid) && (want == "" || s == want)) { gsub(/"/, "", $NF); print $NF; exit } }'
        ;;
    *)
        for tty in /sys/class/tty/ttyACM*; do
            [ -e "$tty" ] || continue
            dev="$tty/device/.."
            [ "$(cat "$dev/idVendor" 2>/dev/null)" = "$vid" ] || continue
            [ -z "$pid" ] || [ "$(cat "$dev/idProduct" 2>/dev/null)" = "$pid" ] || continue
            [ -z "$want" ] || [ "$(tr '[:lower:]' '[:upper:]' < "$dev/serial" 2>/dev/null)" = "$want" ] || continue
            echo "/dev/$(basename "$tty")"
            return
        done
        ;;
    esac
}

APP="$(usb_port 1999 0515 "$SERIAL")"   # FSK-WL application
BOOT="$(usb_port 239a)"                 # Adafruit nRF52 bootloader
TOUCH=""
if [ -n "$PORT" ]; then
    [ "$PORT" = "$APP" ] && TOUCH="--touch 1200"
elif [ -n "$APP" ]; then
    PORT="$APP"
    TOUCH="--touch 1200"
elif [ -n "$SERIAL" ]; then
    echo "no FSK-WL board with serial $SERIAL. A board already in the bootloader has another serial: pass its port instead." >&2
    exit 1
elif [ -n "$BOOT" ]; then
    PORT="$BOOT"
else
    # Unknown bootloader id: accept a single remaining port.
    set -- $(ls /dev/ttyACM* /dev/cu.usbmodem* 2>/dev/null || true)
    if [ $# -eq 1 ]; then
        PORT="$1"
    elif [ $# -eq 0 ]; then
        echo "no board found. Plug it in; a new board or a hung app needs a double-tap on RST first." >&2
        exit 1
    else
        echo "several serial ports found; pass the board's port (./flash.sh /dev/ttyACM0) or its chip id (--serial)" >&2
        exit 1
    fi
fi

if ! command -v adafruit-nrfutil >/dev/null 2>&1; then
    echo "installing adafruit-nrfutil ..."
    if command -v pipx >/dev/null 2>&1; then
        pipx install adafruit-nrfutil
    else
        python3 -m pip install --user adafruit-nrfutil
        PATH="$HOME/.local/bin:$PATH"
    fi
fi

if [ -n "$TOUCH" ]; then
    echo "rebooting the app on $PORT into the bootloader, then flashing fsk-timekeeper-dfu.zip"
else
    echo "flashing fsk-timekeeper-dfu.zip via $PORT"
fi
# shellcheck disable=SC2086
if ! adafruit-nrfutil dfu serial --package fsk-timekeeper-dfu.zip -p "$PORT" -b 115200 --singlebank $TOUCH; then
    echo "flashing failed. If the app did not respond, double-tap RST and run this script again." >&2
    exit 1
fi
