"""Capture serial output from the board for a fixed window (non-interactive).

Usage: python serial_capture.py [PORT] [SECONDS]
Defaults: COM4, 30s. Prints each line with a timestamp and exits.
Used in place of `idf.py monitor` (which is interactive/blocking) so Claude Code
can read boot logs and RMS output while you hold the PTT button.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

# 115200 is the ESP-IDF default console baud. Short read timeout so we can
# enforce the overall window even when the board is silent.
ser = serial.Serial(port, 115200, timeout=0.2)
start = time.monotonic()
buf = b""
print(f"[capture] {port} @115200 for {secs:.0f}s", flush=True)
while time.monotonic() - start < secs:
    data = ser.read(4096)
    if not data:
        continue
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = time.monotonic() - start
        print(f"[{t:5.1f}] {line.decode('utf-8', 'replace').rstrip()}", flush=True)
ser.close()
print("[capture] done", flush=True)
