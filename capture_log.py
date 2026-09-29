#!/usr/bin/env python3
"""Capture a device's USB serial log to a file. Usage: capture_log.py OUT SECONDS [USB_SERIAL]
With a USB serial (e.g. 48:CA:43:54:20:E4) it finds that device's port, so two connected devices don't get mixed up.
Leaves DTR/RTS alone: pulsing them (what setting them False does) resets the chip on every open."""
import sys, time, glob, subprocess, serial

def find_port(want):
    if not want:
        return '/dev/ttyACM0'
    for p in sorted(glob.glob('/dev/ttyACM*')):
        try:
            out = subprocess.run(['udevadm', 'info', p], capture_output=True, text=True).stdout
        except Exception:
            continue
        for line in out.splitlines():
            if 'ID_SERIAL_SHORT=' in line and line.split('=', 1)[1].strip().upper() == want.upper():
                return p
    return None

out = open(sys.argv[1], 'w')
end = time.time() + float(sys.argv[2])
want = sys.argv[3] if len(sys.argv) > 3 else None
while time.time() < end:
    port = find_port(want)
    if not port:
        time.sleep(0.1)
        continue
    try:
        s = serial.Serial(port, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
    except Exception:
        time.sleep(0.1)
        continue
    try:
        while time.time() < end:
            data = s.read(256)
            if data:
                out.write(data.decode('utf8', 'replace'))
                out.flush()
    except Exception:
        pass
    finally:
        try:
            s.close()
        except Exception:
            pass
