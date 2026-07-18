# CrowPanel screenshot relay

The relay keeps one validated CrowPanel capture in memory and serves that cache
without making every viewer wait for the panel's slow Wi-Fi transfer. Concurrent
refreshes join one fetch and receive its actual success or failure.

## Security defaults

- Listens on `127.0.0.1`, not every interface.
- Refuses a non-loopback bind unless `RELAY_TOKEN` is set.
- Requires authentication on every route when a token is configured.
- Uses POST for refresh and wake actions. Legacy mutating GET routes return 405.
- POST actions require `X-CrowPanel-Action: 1`, preventing cross-site forms
  from reusing a browser's cached Basic credentials.
- Validates BMP type, dimensions and size before replacing the cache.
- Bounds concurrent clients, request bodies and action frequency.
- Does not enable CORS unless one exact `CORS_ALLOW_ORIGIN` is configured.

The relay accepts `Authorization: Bearer <token>`. It also accepts HTTP Basic
with any username and the token as the password, allowing a browser to show its
normal login prompt. These controls protect access but do not encrypt HTTP; use
Tailscale, a private VLAN, or an HTTPS reverse proxy.

## Endpoints

Read-only:

- `GET /` - preview
- `GET /capture.bmp` and `GET /capture.png` - cached image
- `GET /health` - cache/fetch status

Actions:

- `POST /refresh` - start a refresh
- `POST /refresh?wait=1` - refresh and return the real result
- `POST /wake-controls` - reveal the panel controls
- `POST /controls.bmp` and `POST /controls.png` - wake, refresh and return

For a temporary migration only, `ALLOW_LEGACY_GET_ACTIONS=1` restores the old
GET actions. Do not leave that enabled; crawlers are remarkably gifted at
pressing buttons nobody invited them to press.

## Install on the hub

Install the script read-only and create a root-owned private environment file:

```bash
sudo install -d -m 0755 /opt/crowpanel-relay
sudo install -m 0755 tools/pi_screenshot_relay/relay.py /opt/crowpanel-relay/relay.py
sudo install -m 0644 tools/pi_screenshot_relay/crowpanel-relay.service /etc/systemd/system/
sudo sh -c 'umask 077; : > /etc/crowpanel-relay.env'
sudoedit /etc/crowpanel-relay.env
```

Example `/etc/crowpanel-relay.env` (replace every placeholder locally):

```ini
PANEL_HOST=panel.lan
PANEL_PORT=8080
PANEL_AUTH_USER=crowpanel
PANEL_AUTH_TOKEN=replace-with-device-diagnostics-token
BIND_HOST=100.x.y.z
BIND_PORT=9108
RELAY_TOKEN=replace-with-a-different-random-token
AUTO_REFRESH_SEC=120
```

Generate tokens locally with a password manager or `openssl rand -hex 32`.
Never commit the resulting file. The panel token must match its private
`SCREENSHOT_HTTP_AUTH_TOKEN` setting.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now crowpanel-relay
sudo systemctl status crowpanel-relay --no-pager
```

## Use

```bash
curl -H "Authorization: Bearer $RELAY_TOKEN" http://100.x.y.z:9108/health
curl -H "Authorization: Bearer $RELAY_TOKEN" -o capture.png http://100.x.y.z:9108/capture.png
curl -H "Authorization: Bearer $RELAY_TOKEN" -H "X-CrowPanel-Action: 1" -X POST 'http://100.x.y.z:9108/refresh?wait=1'
```

Open the same address in a browser and enter any username plus the relay token
as the password. Keep tokens out of shell history when possible.

## Important variables

| Variable | Default | Purpose |
|---|---:|---|
| `MAX_CAPTURE_BYTES` | 2000000 | Maximum panel response |
| `MAX_SERVER_THREADS` | 8 | Concurrent client cap |
| `ACTION_MIN_INTERVAL_SEC` | 2 | Per-client action rate limit |
| `REQUEST_TIMEOUT_SEC` | 15 | Socket/header/body timeout |
| `FETCH_TIMEOUT` | 420 | Slow panel fetch timeout |
| `EXPECTED_WIDTH` / `EXPECTED_HEIGHT` | 800 / 480 | Capture validation |
| `CORS_ALLOW_ORIGIN` | unset | One exact allowed origin |

Logs are available with `journalctl -u crowpanel-relay`.
