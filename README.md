# LawnBot CrowPanel display

Firmware and support tools for an Elecrow 7-inch ESP32-S3 panel used as a
LawnBot irrigation status and control surface. The panel renders state and
sends bounded, authenticated commands to the controller hub; the hub remains
the source of truth for schedules, sensors, policy and physical actuation.

## Current safety posture

- Mutating hub requests require a private bearer credential.
- Source schedules larger than the panel's legacy three-zone view preserve
  hidden sets losslessly; saves edit only modeled fields and warn about the
  additional hub-managed data.
- Disconnected or expired hub/weather data is marked stale.
- HTTP screenshots require authentication, are rate-limited, and run outside
  the LVGL loop.
- Device/network secrets live in ignored private configuration.

The display still has a fixed three-zone presentation. Expanding it to all hub
zones is a planned UI/data-model migration, not something schedule saving should
pretend has already happened.

Schedule saves currently use a bounded preflight GET and refuse when the source
has changed. That narrows the race but cannot make GET-plus-PUT atomic. True
cross-client conflict protection requires a hub contract with a revision/ETag
and conditional `If-Match` update; that backend change is deliberately deferred.

## Configure safely

```powershell
Copy-Item include/config_private.example.h include/config_private.h
```

Fill in the ignored file locally. Verify it will not be committed:

```powershell
git check-ignore include/config_private.h
python tools/check_tracked_secrets.py
```

Never put credentials or device-specific addresses in `platformio.ini`,
documentation, screenshots, or command examples. The OTA build reads
`CROWPANEL_OTA_HOST` and `CROWPANEL_OTA_PASSWORD` from the local environment.

## Build

Install PlatformIO, then run:

```powershell
pio run -e crowpanel-7inch
```

Available environments:

| Environment | Purpose |
|---|---|
| `crowpanel-7inch` | Known-good field baseline; may consume private config |
| `crowpanel-7inch-demo` | Safe deterministic demo/CI build; ignores private config |
| `crowpanel-7inch-live-ci` | Credential-free compile of all LIVE code; never deploy |
| `crowpanel-7inch-ota` | Known-good build plus ArduinoOTA upload transport |
| `crowpanel-7inch-platform-next` | Staged PlatformIO framework upgrade |
| `crowpanel-7inch-lovyangfx-next` | Staged LovyanGFX upgrade on known-good framework |
| `crowpanel-7inch-combined-next` | Combined framework plus LovyanGFX candidate |

The `*-next` environments are experiments. A successful compile is not proof
that RGB DMA, touch, OTA rollback, reconnects, or screenshots work on hardware.

Staged compile validation on 2026-07-17:

| Environment | Resolved core/display | RAM | Flash |
|---|---|---:|---:|
| `crowpanel-7inch-platform-next` | Arduino-ESP32 2.0.17 / LovyanGFX 1.1.16 | 39.7% | 46.1% |
| `crowpanel-7inch-lovyangfx-next` | Arduino-ESP32 2.0.6 / LovyanGFX 1.2.25 | 38.3% | 40.1% |
| `crowpanel-7inch-combined-next` | Arduino-ESP32 2.0.17 / LovyanGFX 1.2.25 | 39.7% | 46.6% |

All compile successfully with LVGL 8.4.0. The platform-only build emits a
`REG_SPI_BASE` redefinition warning from LovyanGFX 1.1.16; 1.2.25 removes that
warning. These are private-config-free demo-build snapshots taken while the
shared remediation branch was changing, so use a fresh same-commit matrix for
precise size comparisons and hardware promotion.

## Verify

```powershell
python -m unittest discover -s tests -v
python -m compileall -q tools tests
node --check tools/review_ui/app.js
python tools/check_tracked_secrets.py
python tools/check_firmware_size.py --environment crowpanel-7inch
```

CI builds both the private-config-free demo environment and a credential-free
LIVE environment, enforcing a 90% OTA partition ceiling on each. The LIVE-CI
binary exists only to compile networking/control/OTA paths and must never be
deployed: its credentials are intentionally empty. This leaves rollback and
maintenance headroom instead of treating the final few flash sectors as a
luxury penthouse.

## Diagnostics and deployment

- [NETWORKING.md](NETWORKING.md) describes network roles without credentials.
- [HUB_CHECKLIST.md](HUB_CHECKLIST.md) describes the panel/hub API boundary.
- [SECURITY.md](SECURITY.md) covers credential and disclosure practices.
- [tools/pi_screenshot_relay/README.md](tools/pi_screenshot_relay/README.md)
  documents the authenticated cache relay.
- `.cursor/skills/deploy-opps/SKILL.md` contains the deployment checklist.

## Project status and license

This repository does not currently declare a software license. That is a user
or business decision; choose and add one before public distribution or accepting
outside contributions. No license was invented by the remediation work because
law via surprise text file is generally frowned upon.
