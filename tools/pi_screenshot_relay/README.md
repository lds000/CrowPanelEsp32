# CrowPanel Screenshot Relay (hub Pi side)

A small Python HTTP proxy that exposes the CrowPanel's on-device
`/capture.bmp` endpoint via the hub Pi's Tailscale IP, so remote
tailnet members can fetch live screenshots without relying on
Tailscale's subnet-routing (which has been flaky from some hosts).

```
┌──────────────┐  Tailscale  ┌──────────┐   LAN   ┌───────────┐
│ Remote PC    │────100.x───▶│ Hub Pi   │────────▶│ CrowPanel │
│ (anywhere)   │             │ 9108     │ :8080   │ .107      │
└──────────────┘             └──────────┘         └───────────┘
```

## Endpoints (once deployed)

- `http://<hub-pi>:9108/` — HTML preview with auto-refresh
- `http://<hub-pi>:9108/capture.bmp` — raw BMP passthrough
- `http://<hub-pi>:9108/capture.png` — PNG-converted (smaller, browser-native)
- `http://<hub-pi>:9108/health` — JSON relay + panel status

From the remote PC, use the hub Pi's Tailscale IP (`100.116.147.6`):

```powershell
curl -o capture.png http://100.116.147.6:9108/capture.png
start capture.png
```

Or open `http://100.116.147.6:9108/` in a browser with auto-refresh on.

## Deploy to the Pi

From this repo root, with SSH access to `lds00@192.168.68.88`:

```bash
# Copy relay + systemd unit
ssh lds00@192.168.68.88 'mkdir -p ~/crowpanel-relay'
scp tools/pi_screenshot_relay/relay.py lds00@192.168.68.88:~/crowpanel-relay/
scp tools/pi_screenshot_relay/crowpanel-relay.service lds00@192.168.68.88:/tmp/

# Install (one-time)
ssh lds00@192.168.68.88 '
  sudo apt install -y python3-pil &&
  sudo cp /tmp/crowpanel-relay.service /etc/systemd/system/ &&
  sudo systemctl daemon-reload &&
  sudo systemctl enable --now crowpanel-relay &&
  sudo systemctl status crowpanel-relay --no-pager
'
```

## Operational

Check health:
```bash
curl http://192.168.68.88:9108/health
```

Logs:
```bash
ssh lds00@192.168.68.88 'journalctl -u crowpanel-relay -n 50 --no-pager'
```

Restart after panel firmware change:
```bash
ssh lds00@192.168.68.88 'sudo systemctl restart crowpanel-relay'
```

## Prerequisite

The CrowPanel firmware must have `ENABLE_SCREENSHOT_HTTP 1` at build
time (it is, since commit `8c19648`). The relay will return HTTP 502
with "panel fetch failed" until the new firmware is flashed.
