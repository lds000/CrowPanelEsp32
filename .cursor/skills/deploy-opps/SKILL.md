---
name: deploy-opps
description: >-
  Deploy and verify firmware to the LawnBot CrowPanel ESP32 display, via
  WiFi OTA (ArduinoOTA + HTTP browser upload) or USB serial. Use when the
  user wants to flash, upload, OTA, deploy, push firmware, update the
  CrowPanel, or troubleshoot an OTA that won't connect, won't authenticate,
  or hangs after auth. Encodes the project's known footguns (LovyanGFX/LVGL
  version pinning, DEMO/LIVE config secrets, Tailscale subnet routing,
  required first-time USB flash) so subsequent deploys avoid the same
  failures.
---

# Deploy Ops — LawnBot CrowPanel

This project flashes an Elecrow CrowPanel 7" (ESP32-S3-WROOM-1-N4R8) over
either USB serial (CH340) or WiFi OTA. WiFi OTA only works after a
prerequisite USB flash that contains the OTA listener.

## Quick reference

| Channel | Endpoint | Use when |
|---|---|---|
| USB serial | `pio run -e crowpanel-7inch --target upload --upload-port COM8` | First-time flash, recovery, or device has no OTA listener yet |
| ArduinoOTA (PIO espota) | `pio run -e crowpanel-7inch-ota --target upload` | Routine WiFi update from a host on the same LAN with no Tailscale subnet shadow |
| HTTP browser OTA | `curl.exe -F firmware=@firmware.bin http://192.168.68.107/update` | Routine WiFi update from any host (even with Tailscale interfering); also usable from a phone browser |

Device defaults: `192.168.68.107`, hostname `crowpanel.local`, ArduinoOTA UDP port `3232`, HTTP OTA port `80`, OTA password `lawnbot`.

## Pre-flight checklist (always run before flashing)

```
- [ ] Working tree clean OR you've reviewed every uncommitted change
- [ ] include/config.h is in LIVE mode with real WiFi creds (locally only — DO NOT COMMIT)
- [ ] Device pings: `ping -n 2 192.168.68.107`
- [ ] OTA listener up (only for OTA path): http://192.168.68.107/ returns the OTA page
- [ ] Firmware binary contains expected strings (build verification step below)
```

## Step 1 — set the LIVE config locally (never commit)

The committed `include/config.h` ships in `APP_MODE_DEMO` with placeholder
WiFi credentials so the project stays clean for newcomers. Before any
deploy, edit it locally to the LIVE values for this device:

```c
#define APP_MODE       APP_MODE_LIVE
#define WIFI_SSID      "stolnet2"
#define WIFI_PASSWORD  "magpie11"
#define LAWNBOT_HOST   "192.168.68.88"
```

After the deploy completes, **revert immediately**:

```powershell
git checkout -- include/config.h
```

If you accidentally stage or commit it, scrub it before pushing — the WiFi
password is plaintext in the binary anyway, but should not be in git.

## Step 2 — build and verify the binary

```powershell
pio run -e crowpanel-7inch
```

Then confirm the built `firmware.bin` contains what you expect. This catches
"flashed the wrong config" bugs before they brick a remote device:

```powershell
$bytes = [System.IO.File]::ReadAllBytes(".pio\build\crowpanel-7inch\firmware.bin")
$text  = [System.Text.Encoding]::ASCII.GetString($bytes)
foreach ($s in @('stolnet2','192.168.68.88','[OTA]','ArduinoOTA','lawnbot')) {
    "{0,-20} {1}" -f $s, $text.Contains($s)
}
```

All five must be `True`. If any are `False`, do not flash.

## Step 3 — flash

### A. USB serial (first-time, recovery, or no listener yet)

```powershell
pio device list                                         # confirm CH340 on COM8
pio run -e crowpanel-7inch --target upload --upload-port COM8
```

Look for `Hash of data verified.` in the output. The device hard-resets via
RTS automatically; no buttons needed (the CH340 handles boot mode).

### B. ArduinoOTA (PIO espota over UDP/TCP, port 3232)

```powershell
pio run -e crowpanel-7inch-ota --target upload
```

Configured in `platformio.ini` `[env:crowpanel-7inch-ota]`. Auth password
matches `OTA_PASSWORD` in `include/config.h`.

If this hangs at `Waiting for device...`, see the **Tailscale routing**
gotcha below — switch to the HTTP path.

### C. HTTP browser upload (most reliable from this Windows host)

```powershell
curl.exe -F "firmware=@.pio/build/crowpanel-7inch/firmware.bin" `
         http://192.168.68.107/update --max-time 180
```

Device reboots ~3 seconds after a successful upload.

## Step 4 — verify

```powershell
ping -n 2 192.168.68.107
$page = (Invoke-WebRequest "http://192.168.68.107/" -TimeoutSec 10 -UseBasicParsing).Content
[regex]::Matches($page,'<span class=''val''>([^<]+)</span>') | % { $_.Groups[1].Value }
```

Confirm the **Build** timestamp matches `(Get-Item .pio\build\crowpanel-7inch\firmware.bin).LastWriteTime`
(within a few seconds — the C `__DATE__`/`__TIME__` is when `ota_server.cpp` was compiled).

## Common issues and fixes

### 1. Build fails — `lv_font_montserrat_48 undeclared`, `lv_img_dsc_t unknown`

**Cause**: LovyanGFX 1.2.x (especially 1.2.20+) was rewritten against the
LVGL v9 API. With `lvgl @ ^8.4.0`, the build chain pulls in LovyanGFX's
`lvgl.h` shim instead of real LVGL.

**Fix (already applied in this repo)**: `platformio.ini` pins
`lovyan03/LovyanGFX @ 1.1.16`. Do not relax this without also migrating to
LVGL v9 in `include/lv_conf.h`.

To recover after a stale libdep:
```powershell
Remove-Item -Recurse -Force .pio\libdeps,.pio\build
pio run -e crowpanel-7inch
```

### 2. Build fails — `undefined reference to ota_server_loop()` in DEMO

**Cause**: `main.cpp` calls `ota_server_loop()` unconditionally, but the
implementation is gated by `APP_MODE_LIVE && ENABLE_OTA`.

**Fix (already applied)**: `src/ota_server.cpp` provides empty `#else`
stubs. If you ever extract OTA into a separate module, keep the stubs.

### 3. OTA hangs at `Waiting for device...` after `Authenticating...OK`

**Cause**: `espota.py` sends UDP to the device, the device authenticates,
then opens a *new TCP back-connection* to the host's source IP. If the
host has Tailscale and Tailscale advertises subnet routes for the LAN
(`192.168.68.0/22` with metric 5 vs LAN metric 291), Windows sources
outbound traffic from `100.87.89.90` (the Tailscale IP). The device tries
to reach that and can't.

**Verify**:
```powershell
Find-NetRoute -RemoteIPAddress 192.168.68.107 |
  Select InterfaceAlias,IPAddress -First 1
```
If `InterfaceAlias` is `Tailscale`, you have the issue.

**Workarounds**:
- **Use the HTTP browser path instead** (`curl ... /update`) — single TCP
  stream, works fine over Tailscale.
- **OTA from another host** without Tailscale subnet routing
  (e.g. the hub Pi at `192.168.68.88`).
- **Add a host-specific route** (admin shell) to force Ethernet:
  `route add 192.168.68.107 mask 255.255.255.255 192.168.68.1 metric 1`
  then `route delete 192.168.68.107` after.
- Last resort: temporarily disable Tailscale's "Use Tailscale subnets"
  setting on this host.

### 4. OTA push fails — `No response from the ESP` on the very first try

**Cause**: the running firmware on the device pre-dates the OTA-server
commit `5581069`. Port 3232 and port 80 are simply not open.

**Verify**:
```powershell
Test-NetConnection 192.168.68.107 -Port 80 -InformationLevel Quiet
```

**Fix**: USB-flash once via COM8 with the OTA-capable build (Step 3.A).
After this single USB upload, all future updates can use OTA.

### 5. Device boots but never connects to WiFi (no OTA, no toast)

**Possible causes** (in priority order):
1. WiFi credentials in `include/config.h` are wrong — most common
2. SSID is on 5 GHz only — ESP32-S3 is 2.4 GHz only
3. Router's MAC filter is blocking the device's new MAC (rare; same chip → same MAC)
4. DHCP exhausted — bounce the router

**Diagnose**: open serial monitor on COM8. Look for `[WiFi] Connecting to <SSID>`
followed by either `[WiFi] IP: x.x.x.x` (success) or `[WiFi] Connection failed`
after 60 seconds of dots:
```powershell
pio device monitor -e crowpanel-7inch -p COM8 -b 115200
```

The boot-time WiFi window is 60 s (`connect_wifi()` in `src/main.cpp`).
If it misses that, `loop()` retries WiFi every 10 s and re-invokes
`ota_server_init()` (idempotent) when WiFi finally comes up — so the device
self-heals as long as WiFi ever associates.

### 6. HTTP OTA upload aborts mid-stream with `Recv failure: Connection was reset`

**Symptoms**: curl shows partial transfer, then `(56) Recv failure`. Device
is still alive (HTTP page still serves).

**Causes**:
- Tailscale path glitch (long-running TCP through the relay drops)
- Heap exhaustion on device (unlikely — Update streams to flash)
- WiFi packet loss

**Fix**: just retry. The `Update` API on the device rolls back on aborted
writes — you cannot brick the device this way. If it fails repeatedly:
1. Switch host (try OTA from the hub Pi)
2. Move the device closer to the AP
3. Reduce upload size (rarely needed; 1.8 MB easily fits)

### 7. Device pingable but every TCP service is closed

You are likely running the *previous* (pre-OTA-commit) firmware. See
issue 4 — USB-flash once.

If USB-flashing also fails (`pio device list` shows no CH340), the device
may have crashed into a boot loop. Power-cycle it via the USB cable.

## Footgun reminders

- **Never commit `include/config.h` with real credentials.** `git diff include/config.h` before every commit when working on deploy-related code.
- **Don't push the OTA env to a device whose committed config is DEMO** — the device will boot, fail to find `Guest`/`Healthy!`, and lose its OTA listener until USB recovery.
- **Don't bump LovyanGFX past 1.1.16** until you've also migrated `lv_conf.h` to LVGL 9 — see issue 1.
- **`pio run --target upload` without `--upload-port`** auto-detects the serial port. Fine when only the CH340 is connected, ambiguous otherwise. Always pass `--upload-port COM8` for USB flashes when other USB-serial devices are plugged in.

## File map

| Path | Purpose |
|---|---|
| `platformio.ini` | Build envs (`crowpanel-7inch`, `crowpanel-7inch-ota`) and dep pins |
| `include/config.h` | WiFi creds, APP_MODE, OTA password — **secrets**, do not commit edits |
| `include/lv_conf.h` | LVGL 8.4 config — must match installed LVGL major version |
| `src/main.cpp` | `connect_wifi()` (60 s window), reconnect loop, `ota_server_init()` re-call |
| `src/ota_server.cpp` | ArduinoOTA + HTTP `/update` handlers; idempotent init |
| `partitions.csv` | App partition is 3 MB; current build uses ~58% |
