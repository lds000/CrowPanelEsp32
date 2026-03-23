#!/usr/bin/env python3
"""
Download a BMP screen capture from the CrowPanel HTTP screenshot server.

  1. Flash firmware with ENABLE_SCREENSHOT_HTTP 1 in include/config.h
  2. Device must join WIFI_SSID (same LAN as this PC)
  3. Run:  python tools/fetch_screenshot.py [device_ip]

If IP is omitted, tries mDNS hostname lawnbot-display.local (may not work on Windows).

Output: tools/device_capture.bmp  (open in any image viewer or Cursor)
"""
import os
import sys
import urllib.request

DEFAULT_PORT = 8080
OUT = os.path.join(os.path.dirname(__file__), "device_capture.bmp")


def main():
    if len(sys.argv) >= 2:
        host = sys.argv[1].strip()
    else:
        host = "lawnbot-display.local"  # user usually passes IP on Windows

    url = f"http://{host}:{DEFAULT_PORT}/capture.bmp"
    print(f"GET {url}")
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            data = r.read()
    except Exception as e:
        print("Failed:", e)
        print("\nUsage: python tools/fetch_screenshot.py <ESP32_IP>")
        print("  Find IP in Serial Monitor after boot: [HTTP] Screenshot: http://...")
        sys.exit(1)

    with open(OUT, "wb") as f:
        f.write(data)
    print(f"Saved {len(data)} bytes -> {OUT}")


if __name__ == "__main__":
    main()
