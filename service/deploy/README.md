# Running the bubble-screen service

The service is a small FastAPI app. It reads one agent's activity from
Paperclip and serves it to the display at `GET /api/now`.

## Docker

```bash
cd service
cp .env.example .env && chmod 600 .env   # fill it in
docker compose -f deploy/compose.yml up -d --build
curl -s -H "Authorization: Bearer $DEVICE_PSK" http://localhost:8000/api/now
```

## Without Docker

```bash
cd service
python -m venv .venv && .venv/bin/pip install -r requirements.txt
set -a; . ./.env; set +a
.venv/bin/uvicorn --factory app.main:from_env --host 0.0.0.0 --port 8000
```

## Notes

- **Agent key:** create a key for the agent you want to show with
  `POST /api/agents/{id}/keys`. It only sees that agent's company. Paperclip
  keys have no read-only scope, so the service's HTTP client refuses every
  method except GET, and the live event socket only receives.
- **Network:** the display talks plain HTTP (TLS is too heavy for the
  ESP32's RAM), so keep the service on your LAN. You can still put a reverse
  proxy with TLS in front of it for browsers. `/` is an unauthenticated status
  page that shows the current task title.
- **Device password:** `DEVICE_PSK` only unlocks `/api/now`. The Paperclip key
  never leaves the server.
