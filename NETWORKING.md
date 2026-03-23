# LawnBot Networking

## Canonical LAN Addresses
- Hub/controller Pi: `192.168.68.88` on `eth0`
- Sensor Pi: `192.168.68.110` on `eth0`
- CrowPanel display: `192.168.68.107` on Wi-Fi

## Secondary Addresses
- Hub/controller Pi: `192.168.68.103` on `wlan0`
- Hub/controller Pi: `100.116.147.6` on `tailscale0`
- Sensor Pi: `192.168.68.102` on `wlan0`
- Sensor Pi: `100.117.254.20` on `tailscale0`

## Policy
- Use wired `eth0` addresses as the canonical on-LAN endpoints.
- Use Tailscale addresses only for remote or off-LAN access.
- Do not use the Pi `wlan0` addresses for fixed local configuration when `eth0` is available.

## CrowPanel Configuration
- `LAWNBOT_HOST` should point to the hub/controller wired address: `192.168.68.88`
- Wi-Fi SSID: `stolnet2`
- CrowPanel currently operates in live mode and connects to the backend over LAN.

## Why `192.168.68.104` Was Wrong
- `192.168.68.104` was another ESP-based device on the LAN.
- It was reachable by ping but did not expose the expected backend API on port `8000`.
- The working backend API was found on the hub/controller Pi at `192.168.68.88` and also on its Wi-Fi interface `192.168.68.103`.

## Recommendations
- Create DHCP reservations for:
  - Hub/controller Pi `eth0` -> `192.168.68.88`
  - Sensor Pi `eth0` -> `192.168.68.110`
  - CrowPanel display -> `192.168.68.107` if desired
- If Wi-Fi is not needed on either Pi, consider disabling `wlan0` to reduce confusion.
- Keep Tailscale enabled for remote maintenance.
