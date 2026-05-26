#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

// ================= NETWORK CONFIG =================
// Fill these in for your environment.
const char* WIFI_SSID     = "PLDTHOMEFIBRxSQ9R";
const char* WIFI_PASSWORD = "GayoFamily@2025!";

// Jetson server discovery. We resolve JETSON_HOSTNAME via mDNS so the bin
// works on any LAN without hard-coding an IP. JETSON_FALLBACK_IP is used
// only if mDNS resolution fails (rare but possible on guest networks that
// block multicast).
const char* JETSON_HOSTNAME    = "kllewai";        // hostname of the Jetson
const uint16_t JETSON_PORT     = 8000;
const char* JETSON_FALLBACK_IP = " ";

// Shared secret sent in the X-Device-Token header. Must match SMARTRASH_TOKEN
// on the server. Change this before deploying.
const char* DEVICE_TOKEN = "bMSvoZcamDRBZUV1PPgR7V5lJzdr9KgzM2JpPKj-whI";

// Stable identifier for this bin. Useful when you add a second one.
const char* DEVICE_ID = "bin-01";

// How often to push a status update to the server, in milliseconds.
const unsigned long REPORT_INTERVAL_MS = 5000;

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= SERVO =================
Servo lidServo;

// ================= PINS =================

// Ultrasonic 1 (Lid Open Sensor)
#define TRIG1 27
#define ECHO1 26

// Ultrasonic 2 (Bin Capacity Sensor)
#define TRIG2 33
#define ECHO2 25

// Other Components
#define SERVO_PIN 18
#define BUZZER_PIN 19
#define SOUND_PIN 5

// ================= VARIABLES =================
long duration;
float distance1;
float distance2;

bool lidOpen   = false;
bool lidHeld   = false;   // remote-commanded "keep lid open"

unsigned long lastReportMs       = 0;
unsigned long lastCommandPollMs  = 0;
unsigned long lastResolveMs      = 0;
unsigned long lidOpenCount       = 0;
unsigned long soundEventCount    = 0;

// Circuit breaker — if the server appears down, stop pounding it.
// Each failed request increments the counter; once we hit the trip threshold,
// we skip network calls until the cool-down expires.
int           consecutiveFailures = 0;
unsigned long backoffUntilMs      = 0;
const int     FAILURE_TRIP        = 2;       // 2 fails in a row = trip
const unsigned long BACKOFF_MS    = 30000UL; // 30s cool-down before retry

// Cached resolved server (e.g. "192.168.1.10:8000"). Empty on boot.
String serverHost = "";

// Forward declarations.
void pollCommand();
void ackCommand(long cmdId);
void executeCommand(const String& action, long cmdId);
String resolveServer(bool force);
String buildUrl(const String& path);
bool   networkAllowed();
void   noteFailure();
void   noteSuccess();

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);

  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SOUND_PIN, INPUT);

  lidServo.attach(SERVO_PIN);

  // Start closed
  lidServo.write(0);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Smart Trash Bin");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  connectWiFi();

  delay(1500);
  lcd.clear();
}

// ================= LOOP =================
void loop() {

  // ======== READ ULTRASONIC 1 ========
  distance1 = getDistance(TRIG1, ECHO1);

  // ======== READ ULTRASONIC 2 ========
  distance2 = getDistance(TRIG2, ECHO2);

  // ======== READ SOUND SENSOR ========
  // Sample several times so a brief KY-038 pulse isn't missed when the loop
  // gets stretched by serial / network work.
  int soundValue = LOW;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(SOUND_PIN) == HIGH) { soundValue = HIGH; break; }
    delayMicroseconds(500);
  }
  if (soundValue == HIGH) soundEventCount++;

  // ================= SERIAL MONITOR =================

  Serial.println("========== SENSOR STATUS ==========");

  Serial.print("Ultrasonic 1 (Lid): ");
  Serial.print(distance1);
  Serial.println(" cm");

  Serial.print("Ultrasonic 2 (Bin Level): ");
  Serial.print(distance2);
  Serial.println(" cm");

  Serial.print("Sound Sensor: ");

  if (soundValue == HIGH)
    Serial.println("SOUND DETECTED");
  else
    Serial.println("NO SOUND");

  Serial.print("Lid Status: ");

  if (lidOpen)
    Serial.println("OPEN");
  else
    Serial.println("CLOSED");

  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  Serial.println("===================================");
  Serial.println();

  bool isFull = (distance2 > 0 && distance2 <= 10);

  // ================= BIN FULL =================

  if (isFull) {

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("BIN STATUS:");

    lcd.setCursor(0,1);
    lcd.print("FULL");

    Serial.println("BIN IS FULL!");

    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);

    maybeReport(isFull, soundValue);
    pollCommand();
    return; // prevents opening
  }

  // ================= OPEN LID =================

  if (!lidHeld && (distance1 <= 20 || soundValue == HIGH)) {

    openLid();

    delay(3000); // Keep lid open for 3 sec

    closeLid();
  }

  maybeReport(isFull, soundValue);
  pollCommand();

  delay(80);
}

// ================= DISTANCE FUNCTION =================

float getDistance(int trigPin, int echoPin) {

  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);

  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH);

  float distance = duration * 0.034 / 2;

  return distance;
}

// ================= OPEN LID =================

void openLid() {

  if (!lidOpen) {

    lidServo.write(150);

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("TRASH BIN");

    lcd.setCursor(0,1);
    lcd.print("OPEN");

    Serial.println("LID OPENED");

    digitalWrite(BUZZER_PIN, HIGH);
    delay(30);
    digitalWrite(BUZZER_PIN, LOW);

    lidOpen = true;
    lidOpenCount++;
  }
}

// ================= CLOSE LID =================

void closeLid() {

  if (lidOpen) {

    lidServo.write(0);

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("TRASH BIN");

    lcd.setCursor(0,1);
    lcd.print("CLOSED");

    Serial.println("LID CLOSED");

    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);

    lidOpen = false;
  }
}

// ================= WIFI =================

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    // Start mDNS so we can resolve the Jetson by hostname (e.g. "kllewai")
    // regardless of which LAN this bin ends up on.
    if (!MDNS.begin(("smarttrash-" + String(DEVICE_ID)).c_str())) {
      Serial.println("mDNS responder failed to start (continuing anyway).");
    }
  } else {
    Serial.println();
    Serial.println("WiFi connect failed (will retry in background).");
  }
}

// ================= REPORT TO SERVER =================

void maybeReport(bool isFull, int soundValue) {
  unsigned long now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) return;
  lastReportMs = now;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }
  if (!networkAllowed()) return;

  HTTPClient http;
  // Short timeouts on LAN — a healthy server replies in <100ms; if it doesn't,
  // the server is down and we shouldn't block the sensor loop for 3 seconds.
  http.setConnectTimeout(500);
  http.setTimeout(800);

  String url = buildUrl("/ingest");
  if (url.length() == 0) {
    Serial.println("No server resolved — skipping report");
    return;
  }

  // Pick HTTP or HTTPS transport based on the URL.
  bool isHttps = url.startsWith("https://");
  WiFiClient    httpClient;
  WiFiClientSecure tlsClient;
  bool ok;
  if (isHttps) {
    tlsClient.setInsecure();   // skip cert pinning
    ok = http.begin(tlsClient, url);
  } else {
    ok = http.begin(httpClient, url);
  }
  if (!ok) {
    Serial.println("HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);

  String body = "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"distance_lid_cm\":" + String(distance1, 1) + ",";
  body += "\"distance_level_cm\":" + String(distance2, 1) + ",";
  body += "\"sound\":" + String(soundValue == HIGH ? "true" : "false") + ",";
  body += "\"lid_open\":" + String(lidOpen ? "true" : "false") + ",";
  body += "\"is_full\":" + String(isFull ? "true" : "false") + ",";
  body += "\"lid_open_count\":" + String(lidOpenCount) + ",";
  body += "\"sound_event_count\":" + String(soundEventCount) + ",";
  body += "\"uptime_ms\":" + String(now) + ",";
  body += "\"rssi\":" + String(WiFi.RSSI());
  body += "}";

  int code = http.POST(body);
  Serial.print("POST ");
  Serial.print(url);
  Serial.print(" -> ");
  Serial.println(code);

  if (code <= 0) { serverHost = ""; noteFailure(); }
  else            { noteSuccess(); }

  http.end();
}

// ================= COMMAND POLLING =================

// Resolve the Jetson's IP via mDNS, with fallback. Cached for ~60s, or
// invalidated whenever a request fails — so a router reboot or LAN change
// is recovered automatically within one report cycle.
String resolveServer(bool force) {
  if (!force && serverHost.length() > 0
      && (millis() - lastResolveMs) < 60000UL) {
    return serverHost;
  }
  if (WiFi.status() != WL_CONNECTED) return "";

  Serial.print("Resolving "); Serial.print(JETSON_HOSTNAME); Serial.println(".local …");
  IPAddress ip = MDNS.queryHost(JETSON_HOSTNAME);
  String host;
  if (ip != INADDR_NONE) {
    host = ip.toString();
    Serial.print("  -> mDNS "); Serial.println(host);
  } else {
    host = String(JETSON_FALLBACK_IP);
    Serial.print("  -> fallback "); Serial.println(host);
  }
  serverHost = host + ":" + String(JETSON_PORT);
  lastResolveMs = millis();
  return serverHost;
}

String buildUrl(const String& path) {
  String h = resolveServer(false);
  if (h.length() == 0) return "";
  return String("http://") + h + path;
}

void pollCommand() {
  unsigned long now = millis();
  // Poll every 5s so network blocking doesn't starve the sensor loop.
  if (now - lastCommandPollMs < 5000) return;
  lastCommandPollMs = now;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!networkAllowed()) return;

  HTTPClient http;
  WiFiClient plain;
  String url = buildUrl(String("/commands?device_id=") + DEVICE_ID);
  if (url.length() == 0) return;

  http.setConnectTimeout(500);
  http.setTimeout(800);
  if (!http.begin(plain, url)) return;
  http.addHeader("X-Device-Token", DEVICE_TOKEN);

  int code = http.GET();
  if (code != 200) {
    if (code <= 0) { serverHost = ""; noteFailure(); }
    http.end();
    return;
  }
  noteSuccess();

  String body = http.getString();
  http.end();

  // Tiny hand-rolled parse — avoids pulling in a JSON library.
  // Expected: {"command":null} OR {"command":{"id":N,"action":"..."}}
  if (body.indexOf("\"command\":null") >= 0) return;

  int idStart = body.indexOf("\"id\":");
  int actStart = body.indexOf("\"action\":\"");
  if (idStart < 0 || actStart < 0) return;
  long cmdId = body.substring(idStart + 5).toInt();
  int actFrom = actStart + 10;
  int actTo   = body.indexOf('"', actFrom);
  if (actTo < 0) return;
  String action = body.substring(actFrom, actTo);

  Serial.print("Command received: "); Serial.print(action);
  Serial.print(" (id="); Serial.print(cmdId); Serial.println(")");
  executeCommand(action, cmdId);
}

void ackCommand(long cmdId) {
  HTTPClient http;
  WiFiClient plain;
  String url = buildUrl("/commands/ack");
  if (url.length() == 0) return;
  http.setConnectTimeout(500);
  http.setTimeout(800);
  if (!http.begin(plain, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  String body = String("{\"command_id\":") + cmdId + "}";
  http.POST(body);
  http.end();
}

// ================= CIRCUIT BREAKER =================

bool networkAllowed() {
  if (millis() < backoffUntilMs) return false;
  return true;
}

void noteFailure() {
  if (++consecutiveFailures >= FAILURE_TRIP) {
    backoffUntilMs = millis() + BACKOFF_MS;
    Serial.println("Server unreachable — backing off 30s");
  }
}

void noteSuccess() {
  consecutiveFailures = 0;
  backoffUntilMs = 0;
}

void executeCommand(const String& action, long cmdId) {
  if (action == "open_lid") {
    lidHeld = true;
    openLid();
  } else if (action == "close_lid") {
    lidHeld = false;
    closeLid();
  } else if (action == "beep") {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(120);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    Serial.print("Unknown command action: "); Serial.println(action);
  }
  ackCommand(cmdId);
}
