# Smart Trash — Jetson server + Cloudflare tunnel

The Jetson runs a small FastAPI server. The ESP32 posts sensor readings to it
over Wi-Fi. A Cloudflare named tunnel exposes the server at your own domain
without opening any ports on your router.

```
  ESP32 ──HTTP POST /ingest──►  Jetson :8000  ──cloudflared──►  https://smartrash.yourdomain.com
                                 (FastAPI)                        (browser dashboard)
```

## 1. Pick a shared device token

Generate something long and random — used by the ESP32 to authenticate.

```bash
python3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

Put it on the Jetson at `/etc/smartrash.env`:

```bash
sudo cp deploy/smartrash.env.example /etc/smartrash.env
sudo nano /etc/smartrash.env        # paste the token
sudo chmod 600 /etc/smartrash.env
```

Use the **same** value in `Overall_ESP_WorkingCode.ino` for `DEVICE_TOKEN`,
and your Wi-Fi SSID/password for `WIFI_SSID` / `WIFI_PASSWORD`. Set
`INGEST_URL` to `https://smartrash.yourdomain.com/ingest` (or the LAN URL for
local testing).

## 2. Install the API on the Jetson

```bash
cd ~/smartrash/server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# Quick smoke test
SMARTRASH_TOKEN=$(grep SMARTRASH_TOKEN /etc/smartrash.env | cut -d= -f2) \
  .venv/bin/uvicorn app:app --host 127.0.0.1 --port 8000
# curl -s http://127.0.0.1:8000/health
```

Install as a service:

```bash
sudo cp deploy/smartrash-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now smartrash-api
sudo systemctl status smartrash-api
```

The service binds to `127.0.0.1:8000` only — Cloudflare reaches it through the
tunnel, not the LAN. If you want to test from another LAN device, change the
`ExecStart` host to `0.0.0.0` temporarily.

## 3. Install cloudflared on the Jetson (arm64)

```bash
curl -L -o cloudflared.deb \
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64.deb
sudo dpkg -i cloudflared.deb
cloudflared --version
```

## 4. Create the named tunnel

One-time login (opens a browser link, pick your zone):

```bash
cloudflared tunnel login
```

Create the tunnel and note the UUID it prints:

```bash
cloudflared tunnel create smartrash
# Created tunnel smartrash with id  3a1b…  (UUID)
# Credentials saved to /home/kllew/.cloudflared/3a1b….json
```

Map a hostname to it (creates the DNS record on Cloudflare for you):

```bash
cloudflared tunnel route dns smartrash smartrash.yourdomain.com
```

## 5. Configure the tunnel

```bash
cp deploy/cloudflared-config.example.yml ~/.cloudflared/config.yml
nano ~/.cloudflared/config.yml
# replace TUNNEL_UUID (twice) and the hostname
```

Test it interactively first:

```bash
cloudflared tunnel run smartrash
# in another terminal:
# curl -s https://smartrash.yourdomain.com/health
```

If `/health` returns `{"ok": true, ...}`, you're done. Press Ctrl-C and install
as a service:

```bash
sudo cp deploy/cloudflared.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cloudflared
sudo systemctl status cloudflared
```

## 6. Flash the ESP32

In the Arduino IDE (or PlatformIO):

1. Install board support: **esp32 by Espressif Systems**.
2. Install libraries: `LiquidCrystal_I2C`, `ESP32Servo`. (`WiFi` and
   `HTTPClient` ship with the ESP32 core.)
3. Open `esp32/Overall_ESP_WorkingCode.ino`.
4. Edit the four constants at the top: `WIFI_SSID`, `WIFI_PASSWORD`,
   `INGEST_URL`, `DEVICE_TOKEN`.
5. Pick the right board (e.g. *ESP32 Dev Module*) and the correct COM port,
   then upload.

Open the Serial Monitor at 115200 baud — you should see `WiFi connected. IP:
…` and a `POST … -> 200` line every five seconds.

## 7. View the dashboard

Open `https://smartrash.yourdomain.com/` in a browser. The page polls
`/status` every 3 seconds and shows fill level, lid state, sound, RSSI, and
last-seen age per device.

## Useful endpoints

| Method | Path                        | Purpose                              | Auth         |
|--------|-----------------------------|--------------------------------------|--------------|
| GET    | `/health`                   | Liveness probe                       | none         |
| POST   | `/ingest`                   | ESP32 reading                        | device token |
| GET    | `/status`                   | Latest reading per device (JSON)     | none         |
| GET    | `/history?device_id=bin-01` | Last N readings (default 100)        | none         |
| GET    | `/`                         | Dashboard HTML                       | none         |

If you want the read endpoints behind auth, the easiest path is **Cloudflare
Access** on the hostname — leave the FastAPI side as-is and Cloudflare gates
the browser side with email or SSO.

## Why this shape

- ESP32 talks plain HTTP to the Jetson over the LAN, but the LAN never has
  to reach the internet — Cloudflare brings the internet **to** the Jetson.
- Cloudflare Tunnel handles TLS, DDoS, and DNS for you. No port-forward, no
  certbot, no static IP.
- SQLite keeps the install simple and survives reboots. If you outgrow it,
  swap the three SQL helpers in `app.py` for Postgres without touching the
  ESP32.
