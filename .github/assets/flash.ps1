# Flash the fsk-timekeeper firmware over the Adafruit nRF52 bootloader's serial DFU.
#
#   .\flash.ps1 [-Serial <devid>] [COMx]
#
# Finds the board by itself. A board running the FSK-WL app is rebooted into the
# bootloader first (1200-baud touch); a board that is already in the bootloader
# (new board, or after a double-tap on RST) is flashed directly. With several
# boards plugged in, -Serial picks the app board whose USB serial number is the
# given chip id (the 16 hex digits of its `I` line).
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
if ($Serial) {
    $apps = @($apps | Where-Object { (Get-UsbSerial $_.PNPDeviceID) -eq $Serial.ToUpper() })
}
$app = $apps | Select-Object -First 1
$touch = @()
if ($Port) {
    if ($app -and $app.DeviceID -eq $Port) { $touch = @("--touch", "1200") }
} elseif ($app) {
    $Port = $app.DeviceID
    $touch = @("--touch", "1200")
} elseif ($Serial) {
    Write-Error "no FSK-WL board with serial $Serial. A board already in the bootloader has another serial: pass its port instead."
} else {
    # No app: look for the Adafruit bootloader, else a single remaining port.
    $boot = $ports | Where-Object { $_.PNPDeviceID -like "*VID_239A*" } | Select-Object -First 1
    if ($boot) { $Port = $boot.DeviceID }
    elseif ($ports.Count -eq 1) { $Port = $ports[0].DeviceID }
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
