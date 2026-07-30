#Requires -Version 5.1
<#
  一键烧录 ESP32-S3-CAM（绕过 pio 的 uv 联网重装超时问题）
  用法：
    powershell -ExecutionPolicy Bypass -File scripts\flash.ps1
    powershell -ExecutionPolicy Bypass -File scripts\flash.ps1 -Port COM9
    powershell -ExecutionPolicy Bypass -File scripts\flash.ps1 -Port COM9 -MonitorSecs 15
#>
param(
    [string]$Port = "",
    [int]$MonitorSecs = 0,
    [string]$Env = "esp32s3cam"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root ".pio\build\$Env"
$py    = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 自动探测端口（VID_303A）
if (-not $Port) {
    $p = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_303A' -and $_.Class -eq 'Ports' -and $_.FriendlyName -match 'COM(\d+)' } | Select-Object -First 1
    if ($p) {
        $Port = ($p.FriendlyName | Select-String 'COM\d+').Matches.Value
        Write-Host "自动探测到端口: $Port" -ForegroundColor Green
    } else {
        throw "找不到 ESP32-S3 (VID_303A) 端口，请手动指定 -Port"
    }
}

$boot = Join-Path $build "bootloader.bin"
$part = Join-Path $build "partitions.bin"
$fw   = Join-Path $build "firmware.bin"
foreach ($f in @($boot,$part,$fw)) {
    if (-not (Test-Path $f)) { throw "缺失 $f，请先运行 pio run 编译" }
}

Write-Host "==> 开始烧录 $Port ..." -ForegroundColor Cyan
& $py -X utf8 -m esptool `
    --chip esp32s3 --port $Port --baud 921600 `
    write-flash -z `
    --flash-mode dio --flash-freq 80m --flash-size 16MB `
    0x0     $boot `
    0x8000  $part `
    0x10000 $fw
if ($LASTEXITCODE -ne 0) { throw "烧录失败" }

Write-Host "==> 烧录完成" -ForegroundColor Green

if ($MonitorSecs -gt 0) {
    Start-Sleep -Seconds 2
    Write-Host "==> 抓取 $MonitorSecs 秒日志..." -ForegroundColor Cyan
    & "C:\Python314\python.exe" (Join-Path $PSScriptRoot "snap_log.py") $Port $MonitorSecs
}
