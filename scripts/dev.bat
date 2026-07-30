@echo off
REM ============================================================================
REM  ESP32-S3-CAM-Tracker  快捷脚本 (CMD 包装)
REM  用法：  dev.bat <command> [port]
REM   e.g.   dev.bat all COM7
REM ============================================================================
setlocal
set CMD=%1
set PORT=%2

if "%CMD%"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev.ps1" help
    goto :eof
)

if "%PORT%"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev.ps1" %CMD%
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev.ps1" %CMD% -Port %PORT%
)
