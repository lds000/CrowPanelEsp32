---
name: deploy-opps
description: >-
  Build, deploy and verify LawnBot CrowPanel ESP32 firmware over USB,
  ArduinoOTA or the browser updater without committing private configuration.
---

# Deploy operations

Read this before uploading. A deployment is complete only after the device
reboots, reconnects, and the intended version is verified. Building the binary
is not deployment; it is merely the computer saying it has opinions.

## Safety rules

1. Never put Wi-Fi, controller bearer, OTA, or screenshot credentials in a
   tracked file or command example.
2. Copy `include/config_private.example.h` to the ignored
   `include/config_private.h` and set device-specific values there.
3. Review `git status --short` and `git diff -- include/config.h platformio.ini`
   before committing or uploading.
4. Keep the default known-good dependency environment for field deployments.
   The `*-next` environments are experiments until hardware-tested.
5. Do not upload a demo build over OTA to a remote panel: it will stop joining
   the live network and require USB recovery.
6. Obtain explicit user authorization immediately before an actual upload.

## Private environment

Set deployment values in the local shell or a private password manager. Do not
paste their values into logs or chat.

```powershell
$env:CROWPANEL_OTA_PASSWORD = Read-Host 'OTA password'
$env:CROWPANEL_OTA_HOST = Read-Host 'Panel hostname or IP'
$env:CROWPANEL_SCREENSHOT_TOKEN = Read-Host 'Screenshot token'
```

The ignored private header should define live mode, Wi-Fi, controller host,
controller bearer token, and a strong OTA password. Confirm that
`include/config_private.h` remains ignored:

```powershell
git check-ignore include/config_private.h
```

## Build and inspect

```powershell
pio run -e crowpanel-7inch
```

Require a successful build, review RAM/flash usage, and run the repository test
suite before uploading. Never search or print the firmware binary for secret
strings; that helpfully converts a private value into terminal history.

## Upload options

Use the actual local port/host without committing it.

### USB recovery or first install

```powershell
pio device list
pio run -e crowpanel-7inch --target upload --upload-port COMx
```

### ArduinoOTA on the same LAN

The OTA environment reads its password from `CROWPANEL_OTA_PASSWORD`:

```powershell
pio run -e crowpanel-7inch-ota --target upload --upload-port panel.lan
```

ArduinoOTA uses a UDP invitation followed by a TCP callback. VPN subnet routes
can select the wrong callback address; use USB or the HTTP uploader when that
happens rather than repeatedly hammering the panel.

### Browser HTTP updater

Use the authenticated updater in a browser. For command-line uploads, read the
password interactively and use a protected curl config or another method that
does not expose it in the command line/process list. HTTP Basic authentication
does not encrypt traffic, so restrict this to a trusted LAN or encrypted tunnel.

## Verification

After upload:

- Confirm the device reappears on the expected network.
- Confirm OTA and authenticated diagnostics respond.
- Confirm the UI shows fresh hub data, not only Wi-Fi connectivity.
- Perform a read-only screenshot or serial version check.
- Do not start/stop irrigation merely to prove a firmware upload unless the user
  explicitly approved that physical action.

## Common failures

- **OTA waits after authenticating:** inspect the route to the panel; a VPN may
  have captured the LAN subnet. Switch transport.
- **No OTA listener:** use one USB flash with an OTA-capable live build.
- **Device disappears after OTA:** a demo/private-config mismatch was probably
  uploaded. Recover by USB with a reviewed live configuration.
- **Dependency-next build succeeds:** that is not hardware qualification. Test
  touch, RGB/DMA, screenshots, OTA, rollback and reconnect behavior.
- **HTTP screenshot returns 401:** set the private diagnostics credential and
  provide it through `CROWPANEL_SCREENSHOT_TOKEN` to the fetch tool.

## Relevant files

| Path | Purpose |
|---|---|
| `platformio.ini` | Known-good, OTA, demo and staged build environments |
| `include/config.h` | Safe tracked defaults |
| `include/config_private.example.h` | Template for ignored device settings |
| `src/ota_server.cpp` | ArduinoOTA and browser update server |
| `src/screenshot_server.cpp` | Authenticated diagnostic capture endpoint |
| `partitions.csv` | OTA application/rollback layout |
