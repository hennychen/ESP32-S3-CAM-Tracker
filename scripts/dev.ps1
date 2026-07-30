# ============================================================================
# ESP32-S3-CAM-Tracker  Quick Dev Script (PowerShell)
#
# Usage:
#   .\scripts\dev.ps1 <command> [-Port COM7] [-Env esp32s3cam]
#
# Commands:
#   check      Check PlatformIO / esptool / serial ports
#   build      Compile firmware
#   flash      Upload firmware
#   monitor    Open serial monitor
#   upload     flash + monitor
#   erase      Erase full flash (also clears saved WiFi)
#   clean      Clean build cache
#   fullclean  Full rebuild (incl. deps)
#   size       Firmware size analysis
#   all        clean -> build -> flash -> monitor
#   reset      Trigger esptool hard reset (needs -Port)
#
# First-time:
#   .\scripts\dev.ps1 check
#   .\scripts\dev.ps1 all -Port COM7
# ============================================================================
param(
    [Parameter(Position = 0)]
    [ValidateSet('check','build','flash','monitor','upload','erase','clean','fullclean','size','all','reset','help')]
    [string]$Command = 'help',

    [string]$Port = '',
    [string]$Env  = 'esp32s3cam'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$script:PIO_EXE  = ''
$script:PIO_ARGS = @()

function Say($msg, $color = 'Cyan') {
    Write-Host ("`n>>> " + $msg) -ForegroundColor $color
}

function Ensure-Pio {
    if ($script:PIO_EXE) { return }
    $pio = Get-Command pio -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pio) {
        $script:PIO_EXE  = $pio.Source
        $script:PIO_ARGS = @()
        return
    }
    $py = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($py) {
        & $py.Source -m platformio --version 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $script:PIO_EXE  = $py.Source
            $script:PIO_ARGS = @('-m','platformio')
            return
        }
    }
    Write-Host "[X] PlatformIO not found. Install with:" -ForegroundColor Red
    Write-Host "    pip install -U platformio" -ForegroundColor Yellow
    exit 1
}

function Invoke-Pio {
    $full = $script:PIO_ARGS + $args
    Write-Host ("$ " + $script:PIO_EXE + ' ' + ($full -join ' ')) -ForegroundColor DarkGray
    & $script:PIO_EXE @full
}

function Cmd-Check {
    Say "Environment check" 'Green'
    Ensure-Pio
    Invoke-Pio --version
    Write-Host ""
    Say "Serial ports"
    Invoke-Pio device list
    Write-Host ""
    Say "Project"
    Write-Host "  Root: $ProjectRoot"
    Write-Host "  Env : $Env"
    if ($Port) { Write-Host "  Port: $Port" } else { Write-Host "  Port: (auto)" }
}

function Cmd-Build     { Ensure-Pio; Say "Build..." 'Green'; Invoke-Pio run -e $Env }
function Cmd-Flash {
    Ensure-Pio; Say "Flash..." 'Green'
    if ($Port) { Invoke-Pio run -e $Env -t upload --upload-port $Port }
    else       { Invoke-Pio run -e $Env -t upload }
}
function Cmd-Monitor {
    Ensure-Pio; Say "Monitor..." 'Green'
    if ($Port) { Invoke-Pio device monitor -e $Env --port $Port }
    else       { Invoke-Pio device monitor -e $Env }
}
function Cmd-Upload    { Cmd-Flash; Cmd-Monitor }
function Cmd-Erase {
    Ensure-Pio; Say "Erase full flash (clears saved WiFi!)" 'Yellow'
    if ($Port) { Invoke-Pio run -e $Env -t erase --upload-port $Port }
    else       { Invoke-Pio run -e $Env -t erase }
}
function Cmd-Clean     { Ensure-Pio; Say "Clean..."     'Green';  Invoke-Pio run -e $Env -t clean }
function Cmd-FullClean { Ensure-Pio; Say "Fullclean..." 'Yellow'; Invoke-Pio run -e $Env -t fullclean }
function Cmd-Size      { Ensure-Pio; Say "Firmware size..." 'Green'; Invoke-Pio run -e $Env -t size }
function Cmd-Reset {
    Say "Trigger reset" 'Green'
    if ($Port) {
        python -m esptool --chip esp32s3 --port $Port --before default_reset --after hard_reset chip_id
    } else {
        Write-Host "Please provide -Port, e.g. .\scripts\dev.ps1 reset -Port COM7" -ForegroundColor Yellow
    }
}
function Cmd-All { Cmd-Clean; Cmd-Build; Cmd-Flash; Cmd-Monitor }

function Cmd-Help {
    Get-Content $PSCommandPath | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
}

switch ($Command) {
    'check'     { Cmd-Check }
    'build'     { Cmd-Build }
    'flash'     { Cmd-Flash }
    'monitor'   { Cmd-Monitor }
    'upload'    { Cmd-Upload }
    'erase'     { Cmd-Erase }
    'clean'     { Cmd-Clean }
    'fullclean' { Cmd-FullClean }
    'size'      { Cmd-Size }
    'all'       { Cmd-All }
    'reset'     { Cmd-Reset }
    default     { Cmd-Help }
}
