# Smart Trash Bin System 🗑️

An intelligent, touchless waste management system using ESP32 microcontroller with automated lid control, real-time capacity monitoring, and cloud-based dashboard.

## 📋 Table of Contents
- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Hardware Components](#hardware-components)
- [ESP32 Wiring & Connections](#esp32-wiring--connections)
- [How It Works](#how-it-works)
- [Software Components](#software-components)
- [Setup Instructions](#setup-instructions)
- [Code Explanation](#code-explanation)
- [Usage](#usage)
- [Troubleshooting](#troubleshooting)

---

## 🎯 Overview

The Smart Trash Bin is an IoT-enabled waste management solution that provides:
- **Touchless Operation**: Automatic lid opening via proximity or voice detection
- **Capacity Monitoring**: Real-time trash level measurement
- **Full Bin Alerts**: Visual and audio notifications when bin is full
- **Remote Monitoring**: Web dashboard accessible from anywhere
- **WiFi Connectivity**: Sends sensor data to cloud server every 5 seconds

### Key Features
✅ Dual trigger system (hand proximity + sound detection)  
✅ Automatic lid control with servo motor  
✅ Real-time LCD status display  
✅ Ultrasonic distance sensors for lid detection and capacity monitoring  
✅ Audio feedback via buzzer  
✅ Web-based dashboard with live updates  
✅ SQLite database for historical data  
✅ Secure remote access via Cloudflare tunnel  

---

## 🏗️ System Architecture

```
┌─────────────┐         WiFi/HTTP          ┌──────────────┐
│   ESP32     │ ────────────────────────► │ FastAPI      │
│ Controller  │  POST /ingest (JSON)       │ Server       │
│             │                            │ (Python)     │
│ - Sensors   │                            │              │
│ - Servo     │                            │ - SQLite DB  │
│ - LCD       │                            │ - REST API   │
│ - Buzzer    │                            │              │
└─────────────┘                            └──────┬───────┘
                                                  │
                                                  │ HTTP
                                                  ▼
                                           ┌──────────────┐
                                           │ Web Dashboard│
                                           │ (HTML/CSS/JS)│
                                           │              │
                                           │ - Live stats │
                                           │ - Graphs     │
                                           │ - History    │
                                           └──────────────┘
```

### Data Flow
1. **ESP32** reads sensors (ultrasonic, sound) every ~80ms
2. **Decision Logic** determines if lid should open (hand detected OR sound detected)
3. **Servo Motor** opens/closes lid with audio feedback
4. **LCD Display** shows current status (OPEN/CLOSED/FULL)
5. **HTTP POST** sends sensor data to server every 5 seconds
6. **Server** stores data in SQLite and serves dashboard
7. **Dashboard** displays real-time bin status and historical data

---

## 🔧 Hardware Components

### Microcontroller
- **ESP32 Development Board (DevKit V1)** - Main controller with WiFi

### Sensors
| Component | Quantity | Purpose |
|-----------|----------|---------|
| HC-SR04 Ultrasonic Sensor | 2 | Sensor 1: Hand detection (lid trigger)<br>Sensor 2: Trash level measurement |
| KY-038 Sound Sensor | 1 | Voice/sound-activated lid opening |

### Actuators & Display
| Component | Purpose |
|-----------|---------|
| SG90 Servo Motor | Automated lid opening/closing (0°-180°) |
| 16×2 I2C LCD Display | Real-time status display |
| Keyestudio Buzzer | Audio feedback for operations |

### Power & Protection
- 5V Power Supply (for servo and sensors)
- USB Power for ESP32
- Voltage Divider Circuit (1kΩ + 2kΩ resistors) for 5V→3.3V level shifting

---

## 🔌 ESP32 Wiring & Connections

### Pin Assignment Table

| Component | Wire/Function | ESP32 GPIO | Notes |
|-----------|---------------|------------|-------|
| **Servo Motor** | Signal | GPIO 18 | PWM control (LEDC channel) |
| | VCC | External 5V | **NOT from ESP32** |
| | GND | GND | Common ground |
| **Ultrasonic 1 (Lid)** | TRIG | GPIO 27 | Trigger pulse |
| | ECHO | GPIO 26 | **Via voltage divider** |
| | VCC | 5V | |
| | GND | GND | |
| **Ultrasonic 2 (Level)** | TRIG | GPIO 33 | Trigger pulse |
| | ECHO | GPIO 25 | **Via voltage divider** |
| | VCC | 5V | |
| | GND | GND | |
| **Sound Sensor** | Digital Out | GPIO 5 | HIGH when sound detected |
| | + | 3.3V | |
| | G | GND | |
| **Buzzer** | Signal | GPIO 19 | PWM or digital |
| | V | 3.3V | |
| | G | GND | |
| **LCD Display** | SDA | GPIO 21 | I2C data |
| | SCL | GPIO 22 | I2C clock |
| | VCC | 5V | |
| | GND | GND | |

### ⚠️ Critical: Voltage Divider for HC-SR04 ECHO Pins

ESP32 GPIO pins are **3.3V only** and **NOT 5V tolerant**. The HC-SR04 ECHO pin outputs 5V, which will damage the ESP32 if connected directly.

**Required Protection Circuit:**
```
HC-SR04 ECHO ──┬── 1kΩ resistor ──┬── ESP32 GPIO
               │                   │
               └── 2kΩ resistor ───┴── GND
```

This creates a voltage divider: 5V × (2kΩ / (1kΩ + 2kΩ)) = 3.33V ✓

### Wiring Diagram (Simplified)

```
                    ESP32 DevKit V1
                   ┌────────────────┐
    Servo Signal ──┤ GPIO 18        │
                   │                │
    US1 TRIG ──────┤ GPIO 27        │
    US1 ECHO ──────┤ GPIO 26        │ (via divider)
                   │                │
    US2 TRIG ──────┤ GPIO 33        │
    US2 ECHO ──────┤ GPIO 25        │ (via divider)
                   │                │
    Sound DO ──────┤ GPIO 5         │
    Buzzer ────────┤ GPIO 19        │
                   │                │
    LCD SDA ───────┤ GPIO 21        │
    LCD SCL ───────┤ GPIO 22        │
                   │                │
    5V ────────────┤ VIN            │
    GND ───────────┤ GND            │
                   └────────────────┘
```

---

## ⚙️ How It Works

### 1. Sensor Reading Loop (Every ~80ms)

```
┌─────────────────────────────────────────┐
│ Read Ultrasonic 1 (Lid Sensor)         │
│ → Distance to hand/object               │
└────────────┬────────────────────────────┘
             │
┌────────────▼────────────────────────────┐
│ Read Ultrasonic 2 (Level Sensor)       │
│ → Distance to trash surface             │
└────────────┬────────────────────────────┘
             │
┌────────────▼────────────────────────────┐
│ Read Sound Sensor (8 samples)          │
│ → HIGH if sound detected                │
└────────────┬────────────────────────────┘
             │
             ▼
        Decision Logic
```

### 2. Decision Logic

```
Is bin full? (distance2 ≤ 10cm)
├─ YES → Keep lid CLOSED
│         Display "BIN STATUS: FULL"
│         Activate buzzer alert
│         Skip lid opening logic
│
└─ NO → Check triggers:
        ├─ Hand detected? (distance1 ≤ 20cm)
        │  OR
        └─ Sound detected? (sensor HIGH)
           │
           ├─ YES → Open lid for 3 seconds
           │         Display "OPEN"
           │         Buzzer beep
           │         Auto-close after 3s
           │
           └─ NO → Keep lid closed
                   Display "CLOSED"
```

### 3. Lid Operation Sequence

```
1. Detect trigger (hand OR sound)
2. Servo rotate to 150° (lid open)
3. LCD: "TRASH BIN" / "OPEN"
4. Buzzer: Short beep (30ms)
5. Wait 3 seconds
6. Servo rotate to 0° (lid closed)
7. LCD: "TRASH BIN" / "CLOSED"
8. Buzzer: Short beep (50ms)
```

### 4. Network Communication

Every 5 seconds, ESP32 sends HTTP POST to server:

```json
POST /ingest
Headers: X-Device-Token: [secret]
Body: {
  "device_id": "bin-01",
  "distance_lid_cm": 15.2,
  "distance_level_cm": 25.8,
  "sound": false,
  "lid_open": false,
  "is_full": false,
  "lid_open_count": 42,
  "sound_event_count": 8,
  "uptime_ms": 3600000,
  "rssi": -45
}
```

### 5. Server Processing

```
Server receives data
    ↓
Validates device token
    ↓
Calculates fill percentage:
  (EMPTY_DISTANCE - current) / (EMPTY - FULL) × 100
    ↓
Stores in SQLite database
    ↓
Dashboard queries latest data
    ↓
Displays on web interface
```

---

## 💻 Software Components

### ESP32 Firmware (`esp32/Overall_ESP_WorkingCode.ino`)

**Libraries Used:**
- `Wire.h` - I2C communication for LCD
- `LiquidCrystal_I2C.h` - LCD display control
- `ESP32Servo.h` - Servo motor PWM control
- `WiFi.h` - WiFi connectivity
- `HTTPClient.h` - HTTP POST requests
- `ESPmDNS.h` - Network service discovery

**Key Functions:**
- `getDistance(trig, echo)` - Ultrasonic distance measurement
- `openLid()` / `closeLid()` - Servo control with feedback
- `connectWiFi()` - WiFi connection with auto-reconnect
- `maybeReport()` - Periodic data transmission to server
- `pollCommand()` - Check for remote commands (future feature)

**Configuration Variables:**
```cpp
WIFI_SSID          // Your WiFi network name
WIFI_PASSWORD      // Your WiFi password
JETSON_HOSTNAME    // Server hostname (mDNS)
JETSON_FALLBACK_IP // Server IP address
DEVICE_TOKEN       // Authentication token
DEVICE_ID          // Unique bin identifier
```

### Python Server (`server/app.py`)

**Framework:** FastAPI (modern async web framework)

**Endpoints:**
- `GET /health` - Server health check
- `POST /ingest` - Receive sensor data from ESP32 (requires token)
- `GET /status` - Latest device status (JSON)
- `GET /history` - Historical sensor data
- `GET /` - Web dashboard (HTML)

**Database Schema (SQLite):**
```sql
readings (
  id, device_id, received_at,
  distance_lid_cm, distance_level_cm,
  sound, lid_open, is_full,
  lid_open_count, sound_event_count,
  uptime_ms, rssi
)

commands (
  id, device_id, action,
  created_at, delivered_at, acked_at
)

device_state (
  device_id, alarm_mode, lid_held, updated_at
)
```

### Web Dashboard (`server/templates/dashboard.html`)

**Features:**
- Real-time device status (online/offline)
- Bin fill level with animated progress bar
- Lid state indicator (open/closed)
- Sensor readings display
- Sound detection events
- WiFi signal strength (RSSI)
- Device uptime
- Auto-refresh capability
- Responsive design (mobile-friendly)
- Dark theme optimized for low-light

---

## 🚀 Setup Instructions

### 1. Hardware Assembly

1. **Wire all components** according to the pin assignment table above
2. **Install voltage dividers** for both HC-SR04 ECHO pins (critical!)
3. **Connect external 5V power** for servo motor (not from ESP32)
4. **Mount servo** to trash bin lid mechanism
5. **Position sensors:**
   - Ultrasonic 1: Outside bin, facing outward (hand detection)
   - Ultrasonic 2: Inside bin, facing downward (level measurement)
6. **Mount LCD** on bin exterior for visibility

### 2. ESP32 Firmware Setup

**Install Arduino IDE:**
1. Download from https://www.arduino.cc/en/software
2. Install ESP32 board support:
   - File → Preferences → Additional Board Manager URLs
   - Add: `https://dl.espressif.com/dl/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" → Install

**Install Required Libraries:**
- LiquidCrystal_I2C
- ESP32Servo

(Tools → Manage Libraries → Search and install)

**Configure and Upload:**
1. Open `esp32/Overall_ESP_WorkingCode.ino`
2. Update lines 10-11 with your WiFi credentials:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   ```
3. Update line 18 with your server IP:
   ```cpp
   const char* JETSON_FALLBACK_IP = "192.168.1.10";
   ```
4. Connect ESP32 via USB
5. Select: Tools → Board → ESP32 Dev Module
6. Select: Tools → Port → (your COM port)
7. Click Upload (→) button
8. Open Serial Monitor (115200 baud) to verify connection

### 3. Server Setup

**On your server (Jetson/Linux/Raspberry Pi):**

```bash
cd server

# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Generate device token
python3 -c "import secrets; print(secrets.token_urlsafe(32))"

# Create environment file
sudo nano /etc/smartrash.env
# Add: SMARTRASH_TOKEN=<your_generated_token>

# Test server locally
SMARTRASH_TOKEN=$(grep SMARTRASH_TOKEN /etc/smartrash.env | cut -d= -f2) \
  .venv/bin/uvicorn app:app --host 0.0.0.0 --port 8000

# Install as systemd service
sudo cp deploy/smartrash-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now smartrash-api
sudo systemctl status smartrash-api
```

**Update ESP32 with the same token:**
Edit line 23 in `Overall_ESP_WorkingCode.ino`:
```cpp
const char* DEVICE_TOKEN = "your_generated_token_here";
```

### 4. Cloudflare Tunnel (Optional - for remote access)

```bash
# Install cloudflared
curl -L -o cloudflared.deb \
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64.deb
sudo dpkg -i cloudflared.deb

# Login and create tunnel
cloudflared tunnel login
cloudflared tunnel create smartrash
cloudflared tunnel route dns smartrash smartrash.yourdomain.com

# Configure tunnel
cp deploy/cloudflared-config.example.yml ~/.cloudflared/config.yml
nano ~/.cloudflared/config.yml  # Update tunnel UUID and hostname

# Install as service
sudo cp deploy/cloudflared.service /etc/systemd/system/
sudo systemctl enable --now cloudflared
```

---

## 📖 Code Explanation

### Distance Measurement Algorithm

```cpp
float getDistance(int trigPin, int echoPin) {
  // Send 10μs trigger pulse
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  // Measure echo pulse duration
  duration = pulseIn(echoPin, HIGH);
  
  // Calculate distance: speed of sound = 340 m/s = 0.034 cm/μs
  // Divide by 2 because sound travels to object and back
  float distance = duration * 0.034 / 2;
  
  return distance;
}
```

### Servo Control with PWM

```cpp
// ESP32 LEDC (LED Control) used for PWM
lidServo.attach(SERVO_PIN);  // Attach to GPIO 18

// 0° = closed, 150° = open
lidServo.write(0);    // Close lid
lidServo.write(150);  // Open lid
```

### WiFi Connection with Auto-Reconnect

```cpp
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Wait up to 15 seconds
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }
  
  // Start mDNS for hostname resolution
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin(("smarttrash-" + String(DEVICE_ID)).c_str());
  }
}

// In main loop: auto-reconnect if disconnected
if (WiFi.status() != WL_CONNECTED) {
  WiFi.reconnect();
}
```

### HTTP POST with Circuit Breaker

```cpp
// Circuit breaker prevents hammering a down server
int consecutiveFailures = 0;
unsigned long backoffUntilMs = 0;

void maybeReport() {
  // Skip if in backoff period
  if (millis() < backoffUntilMs) return;
  
  HTTPClient http;
  http.setConnectTimeout(500);  // Fast timeout for LAN
  http.setTimeout(800);
  
  if (http.begin(url) && http.POST(jsonData) == 200) {
    consecutiveFailures = 0;  // Success - reset counter
  } else {
    consecutiveFailures++;
    if (consecutiveFailures >= 2) {
      // Trip circuit breaker - wait 30 seconds
      backoffUntilMs = millis() + 30000;
    }
  }
}
```

### Sound Sensor Multi-Sampling

```cpp
// Sample 8 times to avoid missing brief pulses
int soundValue = LOW;
for (int i = 0; i < 8; i++) {
  if (digitalRead(SOUND_PIN) == HIGH) {
    soundValue = HIGH;
    break;
  }
  delayMicroseconds(500);
}
```

---

## 📱 Usage

### Normal Operation

1. **Power on** the system (ESP32 and server)
2. **Wait for WiFi connection** (LCD shows "Initializing...")
3. **LCD displays "CLOSED"** when ready
4. **Approach bin** with hand (within 20cm) OR make a sound
5. **Lid opens automatically** with beep
6. **Dispose trash** (3-second window)
7. **Lid closes automatically** with beep
8. **Access dashboard** at `http://SERVER_IP:8000` or your Cloudflare domain

### Full Bin Behavior

When trash level reaches ≤10cm from sensor:
- LCD displays: "BIN STATUS: FULL"
- Buzzer activates (alert beep)
- Lid remains CLOSED (prevents overflow)
- Dashboard shows 100% full
- Empty the bin to resume normal operation

### Monitoring via Dashboard

Open browser and navigate to:
- Local: `http://192.168.1.10:8000` (your server IP)
- Remote: `https://smartrash.yourdomain.com` (if Cloudflare tunnel configured)

Dashboard shows:
- Device online/offline status
- Current bin fill percentage
- Lid state (open/closed)
- Last update timestamp
- Sensor readings
- Sound detection events
- WiFi signal strength
- Device uptime

---

## 🔧 Troubleshooting

### ESP32 Won't Connect to WiFi

**Check:**
- WiFi credentials are correct (case-sensitive!)
- WiFi is 2.4GHz (ESP32 doesn't support 5GHz)
- Router is in range
- Serial Monitor shows connection attempts

**Fix:**
```cpp
// Increase timeout in connectWiFi()
while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
```

### Lid Doesn't Open

**Check:**
- Servo is powered from external 5V (not ESP32)
- Servo signal wire connected to GPIO 18
- Serial Monitor shows "LID OPENED" message
- Servo isn't mechanically stuck

**Test servo separately:**
```cpp
void loop() {
  lidServo.write(0);
  delay(1000);
  lidServo.write(150);
  delay(1000);
}
```

### Ultrasonic Sensor Reads 0 or Random Values

**Check:**
- Voltage divider is correctly wired for ECHO pins
- TRIG and ECHO pins not swapped
- Sensor has clear line of sight (no obstacles)
- Sensor powered from 5V (not 3.3V)

**Test:**
```cpp
void loop() {
  float dist = getDistance(TRIG1, ECHO1);
  Serial.print("Distance: ");
  Serial.println(dist);
  delay(500);
}
```

### Server Shows "Invalid Device Token"

**Fix:**
1. Check token matches in both places:
   - ESP32 code line 23: `DEVICE_TOKEN`
   - Server: `/etc/smartrash.env` → `SMARTRASH_TOKEN`
2. Restart server: `sudo systemctl restart smartrash-api`
3. Re-upload ESP32 code

### Dashboard Shows Device Offline

**Check:**
- Server is running: `sudo systemctl status smartrash-api`
- ESP32 is connected to WiFi (Serial Monitor)
- Server IP is correct in ESP32 code (line 18)
- Firewall allows port 8000
- Test server: `curl http://SERVER_IP:8000/health`

### LCD Shows Garbage Characters

**Check:**
- I2C address is correct (usually 0x27 or 0x3F)
- SDA/SCL pins connected to GPIO 21/22
- LCD powered from 5V
- I2C backpack properly soldered

**Find I2C address:**
```cpp
#include <Wire.h>
void setup() {
  Wire.begin();
  Serial.begin(115200);
  for (byte i = 8; i < 120; i++) {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found I2C device at 0x");
      Serial.println(i, HEX);
    }
  }
}
```

---

## 📚 Additional Documentation

- **[WIFI_SETUP_GUIDE.md](WIFI_SETUP_GUIDE.md)** - Changing WiFi networks
- **[WIFI_QUICK_REFERENCE.txt](WIFI_QUICK_REFERENCE.txt)** - Quick WiFi change card
- **[JETSON_WIFI_SETUP.md](JETSON_WIFI_SETUP.md)** - Server WiFi configuration
- **[FORMAL_PAPER_SECTIONS.md](FORMAL_PAPER_SECTIONS.md)** - Academic documentation
- **[WIRING_ESP32.md](WIRING_ESP32.md)** - Detailed wiring diagrams
- **[server/README.md](server/README.md)** - Server setup details

---

## 🤝 Contributing

Contributions welcome! Areas for improvement:
- Mobile app for remote control
- Multiple bin support in dashboard
- Machine learning for usage patterns
- Battery power option
- Solar panel integration
- MQTT protocol support
- Home Assistant integration

---

## 📄 License

This project is open source and available for educational and personal use.

---

## 👥 Authors

Smart Trash Bin System - IoT Waste Management Solution

---

## 🙏 Acknowledgments

- ESP32 community for excellent libraries
- FastAPI for modern Python web framework
- Cloudflare for secure tunnel solution

---

**Last Updated:** May 26, 2026  
**Version:** 1.0.0  
**Status:** Production Ready ✅
