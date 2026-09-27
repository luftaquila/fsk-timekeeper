# Flash the fsk-timekeeper firmware over the Adafruit nRF52 bootloader's serial DFU.
#
#   .\flash.ps1 [COMx]
#
# Finds the board by itself. A board running the FSK-WL app is rebooted into the
# bootloader first (1200-baud touch); a board that is already in the bootloader
# (new board, or after a double-tap on RST) is flashed directly.
param(
    [string]$Port = ""
)
$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

$ports = @(Get-CimInstance Win32_SerialPort)
$app = $ports | Where-Object { $_.PNPDeviceID -like "*VID_1999&PID_0515*" } | Select-Object -First 1
$touch = @()
if ($Port) {
    if ($app -and $app.DeviceID -eq $Port) { $touch = @("--touch", "1200") }
} elseif ($app) {
    $Port = $app.DeviceID
    $touch = @("--touch", "1200")
} else {
    # No app: look for the Adafruit bootloader, else a single remaining port.
    $boot = $ports | Where-Object { $_.PNPDeviceID -like "*VID_239A*" } | Select-Object -First 1
    if ($boot) { $Port = $boot.DeviceID }
    elseif ($ports.Count -eq 1) { $Port = $ports[0].DeviceID }
    elseif ($ports.Count -eq 0) { Write-Error "no board found. Plug it in; a new board or a hung app needs a double-tap on RST first." }
    else { Write-Error "several serial ports found; pass the board's port: .\flash.ps1 COM5" }
}

if (-not (Get-Command adafruit-nrfutil -ErrorAction SilentlyContinue)) {
    Write-Host "installing adafruit-nrfutil ..."
    if (Get-Command py -ErrorAction SilentlyContinue) { py -m pip install adafruit-nrfutil } else { python -m pip install adafruit-nrfutil }
}

if ($touch.Count) { Write-Host "rebooting the app on $Port into the bootloader, then flashing fsk-timekeeper-dfu.zip" }
else { Write-Host "flashing fsk-timekeeper-dfu.zip via $Port" }
adafruit-nrfutil dfu serial --package fsk-timekeeper-dfu.zip -p $Port -b 115200 --singlebank @touch
if ($LASTEXITCODE -ne 0) {
    Write-Host "flashing failed. If the app did not respond, double-tap RST and run this script again."
    exit $LASTEXITCODE
}
