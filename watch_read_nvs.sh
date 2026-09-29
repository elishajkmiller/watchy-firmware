#!/bin/bash
# Read the NVS partition (0x9000, 0x5000 bytes) from the Watchy as soon as it enumerates. Read-only.
OUT=${2:-$HOME/watchy-backup/nvs-$(date +%H%M%S).bin}
end=$((SECONDS+${1:-600}))
while [ $SECONDS -lt $end ]; do
  if [ -e /dev/ttyACM0 ]; then
    ser=$(udevadm info /dev/ttyACM0 2>/dev/null | sed -n 's/.*ID_SERIAL_SHORT=//p')
    if [ "${ser^^}" = "48:CA:43:54:20:E4" ]; then
      "$HOME/watchy-build/.venv/bin/esptool" --port /dev/ttyACM0 --chip esp32s3 read-flash 0x9000 0x5000 "$OUT" 2>&1 | tail -3
      rc=${PIPESTATUS[0]}; echo "READ_EXIT=$rc out=$OUT"
      [ $rc -eq 0 ] && exit 0
      sleep 1
    fi
  fi
  sleep 0.05
done
echo "timed out"; exit 1
