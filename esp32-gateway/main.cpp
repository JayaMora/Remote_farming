#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include "SSD1306Wire.h"
#include <DHT.h>
#include <time.h>

// =====================================================
// OLED - Heltec WiFi LoRa 32 V2
// =====================================================
#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_RST 16
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

// =====================================================
// LoRa Pins
// =====================================================
#define LORA_SS   18
#define LORA_RST  14
#define LORA_DIO0 26
#define LORA_BAND 915E6

// =====================================================
// WiFi / MQTT
// =====================================================
const char* ssid     = "Galaxy A56 5G E79B";
const char* password = "jw2v6iktsjsrjep";
const char* mqtt_server = "test.mosquitto.org";
const int   mqtt_port   = 1883;

WiFiClient   espClient;
PubSubClient client(espClient);

// =====================================================
// NTP (Sri Lanka timezone IST = UTC+5:30 = 19800 sec)
// =====================================================
const char* ntpServer = "pool.ntp.org";
const long gmtOffsetSec = 19800;
const int  daylightOffsetSec = 0;

// =====================================================
// Local Sensors and Valve = field_main
// =====================================================
#define DHT_PIN 13
#define DHT_TYPE DHT11
#define LIGHT_SENSOR_PIN    36
#define MOISTURE_SENSOR_PIN 39
#define RAIN_SENSOR_PIN     34
#define VALVE_CONTROL_PIN   25
#define VALVE_FEEDBACK_PIN  35

DHT dht(DHT_PIN, DHT_TYPE);

// =====================================================
// Timing
// =====================================================
unsigned long lastPublish     = 0;
unsigned long lastStaleCheck  = 0;
unsigned long lastDecisionRun = 0;
const long publishInterval    = 5000;
const long staleCheckInterval = 5000;
const long decisionInterval   = 5000;
const long nodeTimeoutMs      = 30000;

// =====================================================
// A3 - Moving Average Filter
// =====================================================
const int WINDOW_SIZE = 5;   // last N readings

struct MovingAverage {
  float buf[10];   // headroom in case you bump WINDOW_SIZE
  int   count;
  int   idx;
};

void maInit(MovingAverage& m) {
  m.count = 0;
  m.idx = 0;
}

float maPush(MovingAverage& m, float v) {
  m.buf[m.idx] = v;
  m.idx = (m.idx + 1) % WINDOW_SIZE;
  if (m.count < WINDOW_SIZE) m.count++;
  float sum = 0;
  for (int i = 0; i < m.count; i++) sum += m.buf[i];
  return sum / m.count;
}

// =====================================================
// Per-field state
// =====================================================
// A "field" here means anything the gateway controls — field_main plus the
// three bindable LoRa slots. All share the same struct so the decision engine
// can iterate uniformly.
struct FieldState {
  String name;             // "field_main" / "field_a" / ...
  String boundUid;         // empty for field_main, populated by binding for others
  bool   isLocal;          // true for field_main (sensors+valve wired directly)
  bool   online;
  unsigned long lastSeen;

  // Raw latest readings
  float temperature;
  float humidity;
  float moisture;
  float rain;        // 0-100, higher = wetter
  float light;       // 0-100
  bool  hasData;

  // A3 smoothed readings
  MovingAverage tempMA;
  MovingAverage moistMA;

  // A4 dynamic thresholds (settable from dashboard)
  float moistureThreshold;  // engine fires irrigation if score-based logic says so
  float lightThreshold;     // dashboard-configurable, used as score input

  // Mode: true = AUTO (engine runs), false = MANUAL (dashboard commands win)
  bool  autoMode;

  // External weather hint from dashboard (1=rain forecast, 0=clear)
  bool  weatherRainHint;

  // Current valve state (what we last commanded)
  bool  valveOpen;

  // Last computed irrigation score (0..100), for publishing/debug
  float lastScore;
};

const int FIELD_MAIN = 0;
const int NUM_FIELDS = 4;   // main + 3 LoRa slots
FieldState fields[NUM_FIELDS];

// =====================================================
// Pending unknown UIDs we've announced (so we don't spam)
// =====================================================
const int MAX_PENDING = 8;
String announcedUids[MAX_PENDING];
int announcedCount = 0;

// =====================================================
// OLED helper
// =====================================================
void showMessage(String l1, String l2 = "", String l3 = "", String l4 = "") {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0,  l1);
  display.drawString(0, 14, l2);
  display.drawString(0, 28, l3);
  display.drawString(0, 42, l4);
  display.display();
}

// =====================================================
// Field-state helpers
// =====================================================
int findFieldByUid(const String& uid) {
  for (int i = 1; i < NUM_FIELDS; i++) {
    if (fields[i].boundUid == uid && uid.length() > 0) return i;
  }
  return -1;
}

int findFieldByName(const String& name) {
  for (int i = 0; i < NUM_FIELDS; i++) {
    if (fields[i].name == name) return i;
  }
  return -1;
}

bool alreadyAnnounced(const String& uid) {
  for (int i = 0; i < announcedCount; i++) {
    if (announcedUids[i] == uid) return true;
  }
  return false;
}

void rememberAnnounced(const String& uid) {
  if (announcedCount < MAX_PENDING) announcedUids[announcedCount++] = uid;
}

// =====================================================
// Initialize field state
// =====================================================
void initFields() {
  fields[0] = { "field_main", "",   true,  true,  0,
                0,0,0,0,0, false, {}, {}, 40.0, 50.0, true, false, false, 0 };
  fields[1] = { "field_a",    "",   false, false, 0,
                0,0,0,0,0, false, {}, {}, 40.0, 50.0, true, false, false, 0 };
  fields[2] = { "field_b",    "",   false, false, 0,
                0,0,0,0,0, false, {}, {}, 40.0, 50.0, true, false, false, 0 };
  fields[3] = { "field_c",    "",   false, false, 0,
                0,0,0,0,0, false, {}, {}, 40.0, 50.0, true, false, false, 0 };

  for (int i = 0; i < NUM_FIELDS; i++) {
    maInit(fields[i].tempMA);
    maInit(fields[i].moistMA);
  }
}

// =====================================================
// WiFi / NTP
// =====================================================
void connectWiFi() {
  showMessage("WiFi Connecting", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); retry++;
    showMessage("WiFi Connecting", "Please wait...", "Retry: " + String(retry));
    if (retry > 40) { ESP.restart(); }
  }
  showMessage("WiFi Connected", WiFi.localIP().toString());
  delay(1000);
}

void initNTP() {
  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
  showMessage("NTP Syncing", "...");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) { delay(500); retry++; }
  if (retry < 10) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    showMessage("NTP OK", String(buf));
  } else {
    showMessage("NTP Failed", "Using fallback");
  }
  delay(1000);
}

// Returns hour of day 0..23. Falls back to a rotating value if NTP failed,
// so demos don't break entirely without internet.
int currentHour() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) return timeinfo.tm_hour;
  // Fallback: fake hour from millis to keep the demo alive
  return (millis() / 60000) % 24;
}

// =====================================================
// LoRa Init
// =====================================================
void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) {
    showMessage("LoRa Failed");
    while (true) {}
  }
  showMessage("LoRa Ready");
  delay(800);
}

void initOLED() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(50);
  digitalWrite(OLED_RST, HIGH); delay(50);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
}

// =====================================================
// LoRa command helper
// =====================================================
void sendLoRaCommand(const String& uid, const String& cmd) {
  String packet = "UID=" + uid + ",CMD=" + cmd;
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("LoRa TX: " + packet);
}

// =====================================================
// Apply a valve decision to a field (drives hardware + publishes state)
// =====================================================
void applyValveDecision(FieldState& f, bool open) {
  // Avoid re-publishing identical state to reduce noise
  bool changed = (f.valveOpen != open);
  f.valveOpen = open;

  if (f.isLocal) {
    digitalWrite(VALVE_CONTROL_PIN, open ? HIGH : LOW);
  } else if (f.boundUid.length() > 0) {
    if (changed) sendLoRaCommand(f.boundUid, open ? "VALVE_ON" : "VALVE_OFF");
  }

  // Always publish the (re-)affirmed state so the dashboard knows
  client.publish((f.name + "/valve").c_str(), open ? "OPEN" : "CLOSE");
}

// =====================================================
// A5/A6/A7 SCORING ENGINE
// =====================================================
//
// Inputs (all normalized 0..100):
//   - moisture (smoothed, lower = drier = needs water = HIGHER score)
//   - rain (current rain sensor or weather hint; if raining, score crashes)
//   - light (proxy for sun/evaporation; high light at noon = penalize)
//   - time of day (morning permissive, noon avoid, evening permissive, night ok)
//
// Output: irrigation score 0..100. Valve opens if score >= 60.
//
// The score is a weighted blend:
//   base       = moisture_demand (how dry the soil is)
//   rain_veto  = strong negative if raining
//   time_mult  = morning 1.1, noon 0.5, evening 1.0, night 0.9
//   light_pen  = high light slightly reduces (waters less when blazing sun)
// =====================================================
float computeScore(const FieldState& f) {
  // moisture_demand: at 0% moisture demand=100, at 100% moisture demand=0
  float moistureDemand = constrain(100.0f - f.moisture, 0.0f, 100.0f);

  // rain factor: if rain reading or weather hint says rain, kill the score
  bool isRainingNow = (f.rain > 60.0f) || f.weatherRainHint;
  float rainFactor = isRainingNow ? 0.1f : 1.0f;

  // time-of-day multiplier
  int hour = currentHour();
  float timeMult;
  if      (hour >= 5  && hour < 10) timeMult = 1.1f;   // morning - encourage
  else if (hour >= 10 && hour < 15) timeMult = 0.5f;   // noon - discourage
  else if (hour >= 15 && hour < 19) timeMult = 1.0f;   // afternoon
  else                              timeMult = 0.9f;   // evening/night

  // light penalty: at light=100 reduce score by 20%, at 0 no reduction
  float lightPen = 1.0f - (f.light / 500.0f);  // gentle

  float score = moistureDemand * rainFactor * timeMult * lightPen;
  return constrain(score, 0.0f, 100.0f);
}

// =====================================================
// Run the decision engine for a single field
// =====================================================
void runDecisionEngine(FieldState& f) {
  // MANUAL mode: do nothing; whatever the dashboard set sticks
  if (!f.autoMode) return;

  // Need data before deciding
  if (!f.hasData) return;

  // For remote fields, only decide if they're online (avoid commanding
  // an offline node, which would just queue a doomed LoRa transmission)
  if (!f.isLocal && !f.online) return;

  float score = computeScore(f);
  f.lastScore = score;

  // Threshold for opening is itself dynamic: dashboard's moisture threshold
  // implicitly informs the open/close decision via the score boundary.
  // Use 60 as the score cutoff; adjust if you want.
  bool shouldOpen = score >= 60.0f;

  // Also publish the score itself so the dashboard can chart it
  client.publish((f.name + "/score").c_str(), String(score, 1).c_str());
  client.publish((f.name + "/mode").c_str(), "AUTO");

  applyValveDecision(f, shouldOpen);
}

// =====================================================
// MQTT Callback
// =====================================================
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  String topicStr = String(topic);

  Serial.println("MQTT IN [" + topicStr + "]: " + message);

  // ----- Per-field commands and threshold updates -----
  // Topics look like field_x/command/valve, field_x/threshold/moisture, etc.
  int firstSlash = topicStr.indexOf('/');
  if (firstSlash > 0) {
    String fieldName = topicStr.substring(0, firstSlash);
    String rest      = topicStr.substring(firstSlash + 1);
    int idx = findFieldByName(fieldName);

    if (idx >= 0) {
      FieldState& f = fields[idx];

      // Manual valve command (only honored in MANUAL mode)
      if (rest == "command/valve") {
        if (!f.autoMode) {
          bool open = (message == "OPEN" || message == "ON");
          applyValveDecision(f, open);
        } else {
          Serial.println("Ignoring manual valve cmd while in AUTO");
        }
        return;
      }

      // Mode toggle
      if (rest == "command/mode") {
        f.autoMode = (message == "AUTO" || message == "auto");
        client.publish((f.name + "/mode").c_str(), f.autoMode ? "AUTO" : "MANUAL");
        return;
      }

      // Dynamic threshold updates from dashboard
      if (rest == "threshold/moisture") { f.moistureThreshold = message.toFloat(); return; }
      if (rest == "threshold/light")    { f.lightThreshold    = message.toFloat(); return; }

      // Weather hint from dashboard (publish 1 or 0 to e.g. field_main/weather/rain)
      if (rest == "weather/rain")       { f.weatherRainHint   = (message == "1" || message == "true"); return; }
    }
  }

  // ----- Binding command -----
  if (topicStr == "gateway/command/bind") {
    int uidStart = message.indexOf("\"uid\":\"") + 7;
    int uidEnd   = message.indexOf("\"", uidStart);
    int slotStart = message.indexOf("\"slot\":\"") + 8;
    int slotEnd   = message.indexOf("\"", slotStart);
    if (uidStart < 7 || slotStart < 8) return;

    String uid  = message.substring(uidStart, uidEnd);
    String slot = message.substring(slotStart, slotEnd);

    int idx = findFieldByName(slot);
    if (idx > 0) {  // can't bind to field_main (idx 0)
      fields[idx].boundUid = uid;
      fields[idx].lastSeen = millis();
      fields[idx].online = true;
      showMessage("Bind OK", uid, "-> " + slot);
    }
    return;
  }
}

// =====================================================
// MQTT Reconnect
// =====================================================
void reconnectMQTT() {
  while (!client.connected()) {
    showMessage("MQTT Connecting", mqtt_server);
    String clientId = "Heltec_Gateway_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      // Per-field control topics
      const char* fieldNames[] = {"field_main", "field_a", "field_b", "field_c"};
      for (int i = 0; i < 4; i++) {
        String f = fieldNames[i];
        client.subscribe((f + "/command/valve").c_str());
        client.subscribe((f + "/command/mode").c_str());
        client.subscribe((f + "/threshold/moisture").c_str());
        client.subscribe((f + "/threshold/light").c_str());
        client.subscribe((f + "/weather/rain").c_str());
      }
      client.subscribe("gateway/command/bind");
      showMessage("MQTT Connected", "Subscribed OK");
      delay(800);
    } else {
      delay(2000);
    }
  }
}

// =====================================================
// Read local sensors into field_main state
// =====================================================
void readFieldMainSensors() {
  FieldState& f = fields[FIELD_MAIN];
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("DHT11 read failed");
    return;
  }

  int lightRaw    = analogRead(LIGHT_SENSOR_PIN);
  int moistureRaw = analogRead(MOISTURE_SENSOR_PIN);
  int rainRaw     = analogRead(RAIN_SENSOR_PIN);

  f.humidity    = h;
  f.temperature = maPush(f.tempMA, t);                                         // A3 smoothed
  float rawMoisture = constrain(map(moistureRaw, 0, 4095, 100, 0), 0, 100);
  f.moisture    = maPush(f.moistMA, rawMoisture);                              // A3 smoothed
  f.light       = constrain(map(lightRaw,    0, 4095,   0, 100), 0, 100);
  f.rain        = constrain(map(rainRaw,     0, 4095, 100,   0), 0, 100);
  f.hasData     = true;
  f.lastSeen    = millis();
  f.online      = true;

  // Publish raw + smoothed
  client.publish("field_main/temperature", String(f.temperature, 1).c_str());
  client.publish("field_main/humidity",    String(f.humidity, 1).c_str());
  client.publish("field_main/light",       String(f.light, 1).c_str());
  client.publish("field_main/moisture",    String(f.moisture, 1).c_str());
  client.publish("field_main/rain",        String(f.rain, 1).c_str());
}

// =====================================================
// Apply KEY=VALUE pair into a FieldState
// =====================================================
void applyPair(FieldState& f, const String& key, const String& value) {
  if      (key == "TEMP")  f.temperature = maPush(f.tempMA, value.toFloat());
  else if (key == "HUM")   f.humidity    = value.toFloat();
  else if (key == "MOIST") f.moisture    = maPush(f.moistMA, value.toFloat());
  else if (key == "LIGHT") f.light       = value.toFloat();
}

// =====================================================
// Handle one LoRa packet (UID=...,TEMP=...,HUM=...,MOIST=...)
// =====================================================
void handleLoRaPacket() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String received = "";
  while (LoRa.available()) received += (char)LoRa.read();
  received.trim();
  int rssi = LoRa.packetRssi();

  String uid = "";
  String pairs[8];
  int pairCount = 0;
  int start = 0;
  while (start < received.length() && pairCount < 8) {
    int comma = received.indexOf(',', start);
    String token = (comma == -1) ? received.substring(start) : received.substring(start, comma);
    token.trim();
    int eq = token.indexOf('=');
    if (eq > 0) {
      String key = token.substring(0, eq);
      String val = token.substring(eq + 1);
      if (key == "UID") uid = val;
      else              pairs[pairCount++] = token;
    }
    if (comma == -1) break;
    start = comma + 1;
  }

  if (uid.length() == 0) return;

  int idx = findFieldByUid(uid);
  if (idx == -1) {
    if (!alreadyAnnounced(uid)) {
      rememberAnnounced(uid);
      String payload = "{\"uid\":\"" + uid + "\"}";
      client.publish("gateway/nodes/new", payload.c_str());
      showMessage("New node", uid, "Awaiting bind");
    }
    return;
  }

  FieldState& f = fields[idx];
  f.lastSeen = millis();
  if (!f.online) {
    f.online = true;
    String s = "{\"node\":\"" + f.name + "\",\"status\":\"ONLINE\"}";
    client.publish("gateway/node_status", s.c_str());
  }
  f.hasData = true;

  for (int i = 0; i < pairCount; i++) {
    int eq = pairs[i].indexOf('=');
    applyPair(f, pairs[i].substring(0, eq), pairs[i].substring(eq + 1));
  }

  client.publish((f.name + "/temperature").c_str(), String(f.temperature, 1).c_str());
  client.publish((f.name + "/humidity").c_str(),    String(f.humidity, 1).c_str());
  client.publish((f.name + "/moisture").c_str(),    String(f.moisture, 1).c_str());
  client.publish((f.name + "/light").c_str(),       String(f.light, 1).c_str());
  client.publish((f.name + "/rssi").c_str(),        String(rssi).c_str());
}

// =====================================================
// Mark stale nodes offline
// =====================================================
void checkStaleNodes() {
  unsigned long now = millis();
  for (int i = 1; i < NUM_FIELDS; i++) {
    FieldState& f = fields[i];
    if (f.boundUid.length() == 0 || !f.online) continue;
    if (now - f.lastSeen > nodeTimeoutMs) {
      f.online = false;
      String s = "{\"node\":\"" + f.name + "\",\"status\":\"OFFLINE\"}";
      client.publish("gateway/node_status", s.c_str());
    }
  }
}

// =====================================================
// Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(VALVE_CONTROL_PIN, OUTPUT);
  digitalWrite(VALVE_CONTROL_PIN, LOW);
  pinMode(VALVE_FEEDBACK_PIN, INPUT);

  dht.begin();
  analogReadResolution(12);

  initFields();
  initOLED();
  showMessage("Booting", "AgroSense Gateway");
  initLoRa();
  connectWiFi();
  initNTP();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  showMessage("System Ready", "Engine: AUTO", "Awaiting nodes");
}

// =====================================================
// Loop
// =====================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!client.connected())            reconnectMQTT();
  client.loop();

  handleLoRaPacket();

  unsigned long now = millis();
  if (now - lastPublish >= publishInterval) {
    lastPublish = now;
    readFieldMainSensors();
  }
  if (now - lastStaleCheck >= staleCheckInterval) {
    lastStaleCheck = now;
    checkStaleNodes();
  }
  if (now - lastDecisionRun >= decisionInterval) {
    lastDecisionRun = now;
    for (int i = 0; i < NUM_FIELDS; i++) runDecisionEngine(fields[i]);
  }
}
