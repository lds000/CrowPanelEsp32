#!/usr/bin/env python3
"""
Download a screen capture from the CrowPanel over USB serial (no WiFi).

1. Flash firmware with serial capture (always on).
2. Close any Serial Monitor using the port.
3. Run:
     python tools/fetch_screenshot_serial.py COM3

The script sends "capture\\n", then reads binary: magic "LBSC" + 4-byte LE size + BMP.

Default baud 115200 (~90s for full frame). Use --baud 921600 for ~10s (set the same in firmware
Serial.begin if you change it; default firmware uses 115200).
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    print("Install: pip install pyserial")
    sys.exit(1)

MAGIC = b"LBSC"
OUT = os.path.join(os.path.dirname(__file__), "device_capture.bmp")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="e.g. COM3 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=300)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b"capture\n")
    ser.flush()

    # Skip text lines until binary magic (device prints "[Serial] Capturing…" first)
    buf = b""
    t0 = time.time()
    idx = -1
    while idx < 0 and time.time() - t0 < 30:
        buf += ser.read(4096)
        idx = buf.find(MAGIC)
        if len(buf) > 200000:
            print("Buffer grew without magic — check firmware / baud.")
            ser.close()
            sys.exit(1)
    if idx < 0:
        print("Timeout waiting for LBSC magic. Got (tail):", buf[-200:])
        ser.close()
        sys.exit(1)

    buf = buf[idx + 4 :]
    while len(buf) < 4:
        buf += ser.read(4 - len(buf))

    sz = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)
    buf = buf[4:]
    print(f"Receiving {sz} bytes BMP…")

    data = buf[: min(len(buf), sz)]
    t0 = time.time()
    while len(data) < sz:
        chunk = ser.read(min(65536, sz - len(data)))
        if not chunk:
            if time.time() - t0 > 300:
                print("Timeout")
                ser.close()
                sys.exit(1)
            continue
        data += chunk
        t0 = time.time()
        if len(data) % (256 * 1024) < len(chunk):
            print(f"  … {len(data) // 1024} KiB")

    with open(OUT, "wb") as f:
        f.write(data)
    print(f"Saved -> {OUT}\nOpen in an image viewer or Cursor.")

    # Drain trailing text line "[Serial] Done."
    time.sleep(0.2)
    ser.read(ser.in_waiting or 0)
    ser.close()


if __name__ == "__main__":
    main()
