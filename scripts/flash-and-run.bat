@echo off
REM 一键：编译 + 烧录 + 打开串口监视
REM 可选参数：COM 口，例如   flash-and-run.bat COM7
setlocal
set PORT=%1

REM 自动选择 pio / python -m platformio
where pio >nul 2>nul
if %ERRORLEVEL%==0 (
    set PIO=pio
) else (
    set PIO=python -m platformio
)

if "%PORT%"=="" (
    %PIO% run -t upload && %PIO% device monitor
) else (
    %PIO% run -t upload --upload-port %PORT% && %PIO% device monitor --port %PORT%
)
