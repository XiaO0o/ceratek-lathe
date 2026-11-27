#!/bin/bash
# Ceratek lathe touchscreen firmware: one script for every step (macOS or Linux).
#
#   ./lathe.sh setup            install PlatformIO and download FluidNC v4.1.1 (once, needs internet)
#   ./lathe.sh test             run the PC tests of the lathe logic (needs a C++ compiler)
#   ./lathe.sh build [wifi]     build the firmware (no Wi-Fi by default; "wifi" keeps the web page)
#   ./lathe.sh flash [wifi]     build and write firmware + config_screen.yaml to the board
#   ./lathe.sh config [wifi]    write only the config (after editing config/config_screen.yaml)
#   ./lathe.sh monitor          open the serial console (type $SS, $Heap ...; Ctrl-C to leave)
#   ./lathe.sh restore          put back stock FluidNC v4.1.1 (Wi-Fi) with config_stock.yaml
#
# PORT=/dev/cu.usbserial-130 ./lathe.sh flash    picks the serial port when there is more than one.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/work"
FLUIDNC="$WORK/FluidNC"
PIO="$WORK/venv/bin/pio"
TAG="v4.1.1"

# FluidNC's build runs "python git-version.py"; macOS may have only python3. The venv has both.
export PATH="$WORK/venv/bin:$PATH"

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

env_name() {
    case "${1:-}" in
        "") echo ceratek ;;
        wifi) echo ceratek_wifi ;;
        *) die "unknown option '$1' (use nothing, or 'wifi')" ;;
    esac
}

port_args() {
    if [ -n "${PORT:-}" ]; then echo "--upload-port $PORT"; fi
}

need_setup() {
    [ -x "$PIO" ] && [ -d "$FLUIDNC/.git" ] || die "run './lathe.sh setup' first"
}

# Copy the screen module into the FluidNC checkout, and the chosen config into its file-system image.
prepare() {
    local config="$1"
    cp -R "$HERE/fluidnc/." "$FLUIDNC/"
    cp "$config" "$FLUIDNC/FluidNC/data/config.yaml"
}

cmd_setup() {
    command -v python3 >/dev/null || die "python3 is not installed"
    command -v git >/dev/null || die "git is not installed"
    mkdir -p "$WORK"
    if [ ! -x "$PIO" ]; then
        say "Installing PlatformIO into work/venv"
        python3 -m venv "$WORK/venv"
        "$WORK/venv/bin/python" -m pip install --upgrade pip >/dev/null
        "$WORK/venv/bin/python" -m pip install "platformio>=6.1,<7"
    fi
    if [ ! -d "$FLUIDNC/.git" ]; then
        say "Downloading FluidNC $TAG"
        git clone --depth 1 --branch "$TAG" https://github.com/bdring/FluidNC "$FLUIDNC"
    fi
    say "Setup done. Next: ./lathe.sh build"
}

cmd_test() {
    say "Building and running the PC tests"
    sh "$HERE/host-tests/run.sh"
}

cmd_build() {
    need_setup
    local env; env="$(env_name "${1:-}")"
    prepare "$HERE/config/config_screen.yaml"
    say "Building $env (the first build downloads the ESP32 tools and takes several minutes)"
    (cd "$FLUIDNC" && "$PIO" run -e "$env")
    mkdir -p "$HERE/output"
    cp "$FLUIDNC/.pio/build/$env/firmware.bin" "$HERE/output/firmware-$env.bin"
    say "Built: output/firmware-$env.bin"
}

cmd_flash() {
    need_setup
    local env; env="$(env_name "${1:-}")"
    prepare "$HERE/config/config_screen.yaml"
    say "Writing the firmware ($env). Keep the 12 V supply and USB connected."
    # shellcheck disable=SC2046
    (cd "$FLUIDNC" && "$PIO" run -e "$env" -t upload $(port_args))
    say "Writing the file system with config_screen.yaml"
    # shellcheck disable=SC2046
    (cd "$FLUIDNC" && "$PIO" run -e "$env" -t uploadfs $(port_args))
    say "Done. Unplug the 12 V supply for 5 seconds and plug it back in, then follow the guide."
}

cmd_config() {
    need_setup
    local env; env="$(env_name "${1:-}")"
    prepare "$HERE/config/config_screen.yaml"
    say "Writing only the file system with config_screen.yaml"
    # shellcheck disable=SC2046
    (cd "$FLUIDNC" && "$PIO" run -e "$env" -t uploadfs $(port_args))
    say "Done. Restart the controller (unplug 12 V for 5 seconds)."
}

cmd_monitor() {
    need_setup
    if [ -n "${PORT:-}" ]; then
        "$PIO" device monitor -b 115200 --eol LF --echo -p "$PORT"
    else
        "$PIO" device monitor -b 115200 --eol LF --echo
    fi
}

cmd_restore() {
    need_setup
    prepare "$HERE/config/config_stock.yaml"
    say "Building and writing stock FluidNC $TAG (Wi-Fi), the firmware tested on 2026-11-14"
    # shellcheck disable=SC2046
    (cd "$FLUIDNC" && "$PIO" run -e stock_wifi -t upload $(port_args))
    # shellcheck disable=SC2046
    (cd "$FLUIDNC" && "$PIO" run -e stock_wifi -t uploadfs $(port_args))
    say "Stock FluidNC is back with config_stock.yaml. The screen will stay dark, as before."
}

case "${1:-}" in
    setup) cmd_setup ;;
    test) cmd_test ;;
    build) cmd_build "${2:-}" ;;
    flash) cmd_flash "${2:-}" ;;
    config) cmd_config "${2:-}" ;;
    monitor) cmd_monitor ;;
    restore) cmd_restore ;;
    *) sed -n '2,13p' "$0" ;;
esac
