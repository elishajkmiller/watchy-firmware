#!/bin/bash
# Poll for the Watchy and flash the moment it enumerates (serial-checked, app partition only).
BIN=$HOME/watchy-build/firmware.bin
ES="$HOME/watchy-build/.venv/bin/esptool"
WANT="48:CA:43:54:20:E4"
find_port() { for p in /dev/ttyACM*; do [ -e "$p" ] || continue; ser=$(udevadm info "$p" 2>/dev/null | sed -n 's/.*ID_SERIAL_SHORT=//p'); [ "${ser^^}" = "$WANT" ] && { echo "$p"; return; }; done; }
end=$((SECONDS+${1:-600}))
while [ $SECONDS -lt $end ]; do
  port=$(find_port)
  if [ -n "$port" ]; then
    echo "watch found on $port, flashing"
    $ES --port "$port" --chip esp32s3 write-flash -z 0x10000 "$BIN" 2>&1 | tail -5
    rc=${PIPESTATUS[0]}; echo "FLASH_EXIT=$rc"
    if [ $rc -eq 0 ]; then
      "$HOME/watchy-build/.venv/bin/python" "$HOME/watchy-build/capture_log.py" "$HOME/watchy-backup/boot-log.txt" 75 "$WANT"
      echo "log saved to ~/watchy-backup/boot-log.txt"
      exit 0
    fi
    echo "attempt failed, still watching"; sleep 1
  fi
  sleep 0.05
done
echo "timed out: watch never appeared"; exit 1
