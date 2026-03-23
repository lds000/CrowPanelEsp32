# Hub Side Checklist

## Current Status
- The CrowPanel is already in live mode and talking to the hub at `192.168.68.88`.
- The display is successfully connecting to the hub websocket and reading at least some live state.
- This means a backend is already running on the hub, even if you did not intentionally set one up yourself.

## Canonical Hub Address
- Use `192.168.68.88` as the hub/controller LAN address.
- Treat `192.168.68.103` as the same Pi on `wlan0`, not the canonical service address.
- Reserve `192.168.68.88` in DHCP so the display config stays valid.

## Required Hub Endpoints

### Read Endpoints
- `GET /api/schedule`
- `GET /api/history?limit=14`
- `GET /api/sensors/latest`
- `GET /ws` as a websocket endpoint

### Control Endpoints
- `POST /api/zones/Hanging%20Pots/run`
- `POST /api/zones/Garden/run`
- `POST /api/zones/Misters/run`
- `POST /api/stop-all`
- `PUT /api/schedule`

## Zone Names
- `Hanging Pots`
- `Garden`
- `Misters`

These names must stay consistent between the hub API and the CrowPanel.

## Request Contract Expected By The Display

### Run Zone
- Method: `POST`
- Path: `/api/zones/<zone>/run`
- Content-Type: `application/json`
- Body example:

```json
{"duration_minutes":5}
```

### Stop All
- Method: `POST`
- Path: `/api/stop-all`
- Content-Type: `application/json`
- Body: empty string is acceptable

### Save Schedule
- Method: `PUT`
- Path: `/api/schedule`
- Content-Type: `application/json`
- Body shape:

```json
{
  "schedule": {
    "schedule_days": [true, false, true, false, true, false, true, false, true, false, true, false, true, false],
    "start_times": [
      {
        "time": "06:00",
        "enabled": true,
        "sets": [
          {
            "name": "Hanging Pots",
            "duration_minutes": 10,
            "enabled": true,
            "mode": "normal"
          }
        ]
      }
    ]
  }
}
```

## Response Contract Expected By The Display

### Schedule Response
The display accepts either:
- a root object with `schedule_days` and `start_times`, or
- an object with those fields nested under `schedule`

Example accepted shape:

```json
{
  "schedule_days": [false, true, false, true, false, true, true, true, false, true, false, true, false, true],
  "start_times": [
    {
      "time": "11:10",
      "enabled": true,
      "sets": [
        {
          "name": "Hanging Pots",
          "duration_minutes": 15.0,
          "enabled": true
        }
      ]
    }
  ]
}
```

### History Response
Expected as a JSON array.
Each item should include:
- `set_name` or `name`
- `start_time`
- `duration_seconds`
- `is_manual`
- `completed`

### Sensor Response
The display currently expects `environment` fields like:
- `temperature_c`
- `humidity_percent`
- `wind_speed_ms`
- `wind_direction_compass`

Example expected shape:

```json
{
  "environment": {
    "temperature_c": 24.1,
    "humidity_percent": 52.0,
    "wind_speed_ms": 1.8,
    "wind_direction_compass": "SE"
  }
}
```

Note:
- The live hub currently appears to return sensor data nested under `environment.data` with names like `temperature` and `wind_speed`.
- If weather is missing or wrong on the CrowPanel, this endpoint is the first thing to fix.

### Websocket Status Message
The display listens for websocket messages shaped like:

```json
{
  "type": "status",
  "data": {
    "zone_states": [
      {"name": "Hanging Pots", "relay_on": false},
      {"name": "Garden", "relay_on": true},
      {"name": "Misters", "relay_on": false}
    ],
    "current_run": {
      "set_name": "Garden",
      "remaining_sec": 420,
      "total_sec": 900,
      "is_manual": false
    },
    "next_run": {
      "set_name": "Hanging Pots",
      "scheduled_time": "15:25"
    },
    "schedule_day_index": 6
  }
}
```

The display also tolerates these aliases:
- `name` instead of `set_name`
- `remaining_seconds` instead of `remaining_sec`
- `duration_seconds` instead of `total_sec`
- `time` instead of `scheduled_time`

## Functional Checklist
- Hub backend starts automatically on boot.
- Backend binds on `0.0.0.0:8000`, not just localhost.
- `GET /api/schedule` returns `200 OK`.
- `GET /api/history?limit=14` returns `200 OK`.
- `GET /api/sensors/latest` returns `200 OK`.
- `/ws` accepts websocket connections.
- `POST /api/zones/<zone>/run` actually starts the relay/run.
- `POST /api/stop-all` actually stops active watering.
- `PUT /api/schedule` saves changes persistently.
- Hub survives reboot and still comes back on `192.168.68.88`.

## Recommended Manual Tests
1. On the display, confirm it shows `Live` and `Hub Connected`.
2. Tap `RUN` on one zone and verify the correct relay turns on.
3. Tap `STOP ALL` and verify watering stops.
4. Change one schedule value on the display and save it.
5. Reload the schedule and verify the change persisted.
6. Reboot the hub and confirm the display reconnects automatically.

## If You Want To Clean Up The Hub
- Disable `wlan0` on the hub if you do not need Wi-Fi fallback.
- Keep Tailscale only for remote maintenance.
- Add a simple `/api/health` endpoint that returns version, uptime, and node identity.
