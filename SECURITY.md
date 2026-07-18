# Security policy

## Secrets

Use `include/config_private.h` and environment variables for all Wi-Fi, API,
OTA, and diagnostic credentials. The private header and local environment files
must remain untracked. Run `python tools/check_tracked_secrets.py` before every
handoff.

If a credential is committed, pasted into a public system, or exposed through
process output, rotate it. Removing it from the latest commit does not remove it
from Git history.

## Network exposure

The firmware and relay currently use HTTP on a trusted LAN. Authentication does
not provide encryption. Restrict them with VLAN/firewall policy or an encrypted
tunnel. The Pi relay refuses an unauthenticated non-loopback bind by default.

Do not expose OTA, screenshots, or irrigation control directly to the public
internet. Keep STOP deterministic and available; require authentication and
confirmation for commands that start or alter watering.

## Firmware hardening

Signed OTA, Secure Boot, and flash encryption are valuable production controls,
but key/eFuse changes are irreversible. Introduce them only with tested recovery,
rollback, key custody, and physical USB provisioning procedures.

## Reporting

Report suspected vulnerabilities privately to the repository owner with the
affected version, reproduction steps and impact. Do not include live credentials
or trigger physical irrigation as part of a proof of concept.
