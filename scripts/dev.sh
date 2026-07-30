#!/usr/bin/env bash
# ============================================================================
# ESP32-S3-CAM-Tracker  Quick Dev Script (Bash / MacOS + Linux)
#
# Usage:
#   ./scripts/dev.sh <command> [--port /dev/ttyUSB0] [--env esp32s3cam]
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
#   reset      Trigger esptool hard reset (needs --port)
#   help       Show this help
#
# Typical ports:
#   MacOS  : /dev/tty.usbmodem*   or  /dev/cu.usbmodem*
#   Linux  : /dev/ttyACM0 (native USB-CDC) or /dev/ttyUSB0
#
# First-time:
#   chmod +x scripts/dev.sh
#   ./scripts/dev.sh check
#   ./scripts/dev.sh all --port /dev/ttyACM0
# ============================================================================
set -e

# ---------------- argparse ----------------
CMD="${1:-help}"
shift || true
PORT=""
ENV_NAME="esp32s3cam"

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -e|--env)  ENV_NAME="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---------------- project root ----------------
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"
cd "$PROJECT_ROOT"

# ---------------- colors ----------------
if [ -t 1 ]; then
    C_CYAN="\033[36m"; C_GREEN="\033[32m"; C_YELLOW="\033[33m"
    C_RED="\033[31m"; C_GRAY="\033[90m"; C_RESET="\033[0m"
else
    C_CYAN=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_GRAY=""; C_RESET=""
fi

say()   { printf "\n${C_CYAN}>>> %s${C_RESET}\n" "$1"; }
info()  { printf "${C_GREEN}%s${C_RESET}\n" "$1"; }
warn()  { printf "${C_YELLOW}%s${C_RESET}\n" "$1"; }
error() { printf "${C_RED}%s${C_RESET}\n" "$1" >&2; }

# ---------------- pio detection ----------------
PIO_CMD=()
detect_pio() {
    if [ ${#PIO_CMD[@]} -gt 0 ]; then return; fi
    if command -v pio >/dev/null 2>&1; then
        PIO_CMD=(pio)
    elif command -v platformio >/dev/null 2>&1; then
        PIO_CMD=(platformio)
    else
        PY=""
        for c in python3 python; do
            if command -v "$c" >/dev/null 2>&1; then
                if "$c" -m platformio --version >/dev/null 2>&1; then
                    PY="$c"; break
                fi
            fi
        done
        if [ -n "$PY" ]; then
            PIO_CMD=("$PY" -m platformio)
        else
            error "[X] PlatformIO not found. Install with:"
            warn  "    pip install -U platformio"
            warn  "    (or: brew install platformio  on macOS)"
            exit 1
        fi
    fi
}

pio_run() {
    printf "${C_GRAY}\$ %s${C_RESET}\n" "${PIO_CMD[*]} $*"
    "${PIO_CMD[@]}" "$@"
}

# ---------------- commands ----------------
cmd_check() {
    say "Environment check"
    detect_pio
    pio_run --version
    echo
    say "Serial ports"
    pio_run device list
    echo
    say "Project"
    echo "  Root: $PROJECT_ROOT"
    echo "  Env : $ENV_NAME"
    if [ -n "$PORT" ]; then echo "  Port: $PORT"; else echo "  Port: (auto)"; fi
}

cmd_build()     { detect_pio; say "Build...";     pio_run run -e "$ENV_NAME"; }
cmd_flash() {
    detect_pio; say "Flash..."
    if [ -n "$PORT" ]; then
        pio_run run -e "$ENV_NAME" -t upload --upload-port "$PORT"
    else
        pio_run run -e "$ENV_NAME" -t upload
    fi
}
cmd_monitor() {
    detect_pio; say "Monitor..."
    if [ -n "$PORT" ]; then
        pio_run device monitor -e "$ENV_NAME" --port "$PORT"
    else
        pio_run device monitor -e "$ENV_NAME"
    fi
}
cmd_upload()    { cmd_flash; cmd_monitor; }
cmd_erase() {
    detect_pio; warn ">>> Erase full flash (clears saved WiFi!)"
    if [ -n "$PORT" ]; then
        pio_run run -e "$ENV_NAME" -t erase --upload-port "$PORT"
    else
        pio_run run -e "$ENV_NAME" -t erase
    fi
}
cmd_clean()     { detect_pio; say "Clean...";     pio_run run -e "$ENV_NAME" -t clean; }
cmd_fullclean() { detect_pio; warn ">>> Fullclean..."; pio_run run -e "$ENV_NAME" -t fullclean; }
cmd_size()      { detect_pio; say "Firmware size..."; pio_run run -e "$ENV_NAME" -t size; }
cmd_all()       { cmd_clean; cmd_build; cmd_flash; cmd_monitor; }

cmd_reset() {
    say "Trigger reset"
    if [ -z "$PORT" ]; then
        warn "Please provide --port, e.g. ./scripts/dev.sh reset --port /dev/ttyACM0"
        exit 2
    fi
    PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
    "$PY" -m esptool --chip esp32s3 --port "$PORT" \
        --before default_reset --after hard_reset chip_id
}

cmd_help() { sed -n '1,32p' "$0"; }

case "$CMD" in
    check)     cmd_check ;;
    build)     cmd_build ;;
    flash)     cmd_flash ;;
    monitor)   cmd_monitor ;;
    upload)    cmd_upload ;;
    erase)     cmd_erase ;;
    clean)     cmd_clean ;;
    fullclean) cmd_fullclean ;;
    size)      cmd_size ;;
    all)       cmd_all ;;
    reset)     cmd_reset ;;
    help|-h|--help|"") cmd_help ;;
    *) error "Unknown command: $CMD"; cmd_help; exit 2 ;;
esac
