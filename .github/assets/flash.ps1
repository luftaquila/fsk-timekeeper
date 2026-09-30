# Flash the fsk-timekeeper firmware over the Adafruit nRF52 bootloader's serial DFU.
#
#   .\flash.ps1 [-Serial <devid>] [COMx]
#
# Finds the board by itself. A board running the FSK-WL app is rebooted into the
# bootloader first (1200-baud touch); a board that is already in the bootloader
# (new board, or after a double-tap on RST) is flashed directly. With several
# boards plugged in, -Serial picks the board whose USB serial number is the given
# chip id (the 16 hex digits of its `I` line); the bootloader reports it too. An
# app from before chip-id serials reports 0001 instead.
param(
    [string]$Port = "",
    [string]$Serial = ""
)
$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

# The app is a composite USB device: its COM port is an interface child whose
# parent instance id is USB\VID_1999&PID_0515\<serial>.
function Get-UsbSerial($pnpId) {
    try {
        $parent = (Get-PnpDeviceProperty -InstanceId $pnpId -KeyName 'DEVPKEY_Device_Parent').Data
    } catch {
        $parent = $pnpId
    }
    return ($parent -split '\\')[-1]
}

$ports = @(Get-CimInstance Win32_SerialPort)
$apps = @($ports | Where-Object { $_.PNPDeviceID -like "*VID_1999&PID_0515*" })
$boots = @($ports | Where-Object { $_.PNPDeviceID -like "*VID_239A*" })   # Adafruit nRF52 bootloader
if ($Serial) {
    $apps = @($apps | Where-Object { (Get-UsbSerial $_.PNPDeviceID) -eq $Serial.ToUpper() })
    $boots = @($boots | Where-Object { (Get-UsbSerial $_.PNPDeviceID) -eq $Serial.ToUpper() })
}
$app = $apps | Select-Object -First 1
$boot = $boots | Select-Object -First 1
$touch = @()
if ($Port) {
    if ($app -and $app.DeviceID -eq $Port) { $touch = @("--touch", "1200") }
} elseif ($app) {
    $Port = $app.DeviceID
    $touch = @("--touch", "1200")
} elseif ($boot) {
    $Port = $boot.DeviceID
} elseif ($Serial) {
    Write-Error "no board with serial $Serial. An app from before chip-id serials reports 0001: pass its port, or double-tap RST and run this again."
} else {
    # No app and no bootloader: accept a single remaining port.
    if ($ports.Count -eq 1) { $Port = $ports[0].DeviceID }
    elseif ($ports.Count -eq 0) { Write-Error "no board found. Plug it in; a new board or a hung app needs a double-tap on RST first." }
    else { Write-Error "several serial ports found; pass the board's port (.\flash.ps1 COM5) or its chip id (-Serial)" }
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
