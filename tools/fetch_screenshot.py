#!/usr/bin/env python3
"""
Download a BMP screen capture from the CrowPanel HTTP screenshot server.

  1. Flash firmware with ENABLE_SCREENSHOT_HTTP 1 in include/config.h
  2. Device must join WIFI_SSID (same LAN as this PC)
  3. Run:  python tools/fetch_screenshot.py [device_ip]

If host is omitted, uses CROWPANEL_HOST or the mDNS name crowpanel.local.

Output: tools/device_capture.bmp  (open in any image viewer or Cursor)
"""
import os
import sys
import base64
import struct
import urllib.request

DEFAULT_PORT = 8080
OUT = os.path.join(os.path.dirname(__file__), "device_capture.bmp")
MAX_CAPTURE_BYTES = 2_000_000


def main():
    if len(sys.argv) >= 2:
        host = sys.argv[1].strip()
    else:
        host = os.environ.get("CROWPANEL_HOST", "crowpanel.local")

    url = f"http://{host}:{DEFAULT_PORT}/capture.bmp"
    token = os.environ.get("CROWPANEL_SCREENSHOT_TOKEN", "")
    user = os.environ.get("CROWPANEL_SCREENSHOT_USER", "crowpanel")
    if not token:
        print("Set CROWPANEL_SCREENSHOT_TOKEN to the device screenshot password.")
        sys.exit(2)
    basic = base64.b64encode(f"{user}:{token}".encode("utf-8")).decode("ascii")
    request = urllib.request.Request(url, headers={"Authorization": f"Basic {basic}"})
    print(f"GET {url}")
    try:
        with urllib.request.urlopen(request, timeout=420) as r:
            data = r.read(MAX_CAPTURE_BYTES + 1)
    except Exception as e:
        print("Failed:", e)
        print("\nUsage: python tools/fetch_screenshot.py <ESP32_IP>")
        print("  Find IP in Serial Monitor after boot: [HTTP] Screenshot: http://...")
        sys.exit(1)

    if len(data) > MAX_CAPTURE_BYTES or len(data) < 54 or data[:2] != b"BM":
        print("Failed: device returned an invalid or excessive BMP")
        sys.exit(1)
    if struct.unpack_from("<I", data, 2)[0] != len(data):
        print("Failed: BMP length does not match its header")
        sys.exit(1)

    with open(OUT, "wb") as f:
        f.write(data)
    print(f"Saved {len(data)} bytes -> {OUT}")


if __name__ == "__main__":
    main()
