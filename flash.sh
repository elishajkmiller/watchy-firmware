#!/bin/bash
# Flash the app partition to the Watchy (serial-checked). Needs: python3 -m venv ~/watchy-build/.venv && ~/watchy-build/.venv/bin/pip install esptool platformio
# Build: cd ~/watchy-build && .venv/bin/pio run   ->  .pio/build/watchy_v3/firmware.bin
D=$(dirname "$0"); BIN=${1:-$D/.pio/build/watchy_v3/firmware.bin}; [ -f "$BIN" ] || BIN=$D/firmware.bin
ser=$(udevadm info /dev/ttyACM0 | sed -n 's/.*ID_SERIAL_SHORT=//p')
[ "${ser^^}" = "48:CA:43:54:20:E4" ] || { echo "watch not found (serial=$ser)"; exit 1; }
"$D/.venv/bin/esptool" --port /dev/ttyACM0 --chip esp32s3 write-flash -z 0x10000 "$BIN"
