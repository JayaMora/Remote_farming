#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include "SSD1306Wire.h"
#include <DHT.h>

// =====================================================
// OLED - Heltec WiFi LoRa 32 V2
// =====================================================
#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_RST 16
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

// =====================================================
// LoRa Pins - Heltec WiFi LoRa 32 V2
// =====================================================
#define LORA_SS   18
#define LORA_RST  14
#define LORA_DIO0 26
#define LORA_BAND 915E6

// =====================================================
// WiFi / MQTT - move these to a separate secrets header before publishing to GitHub
// =====================================================
const char* ssid     = "Galaxy A56 5G E79B";
const char* password = "jw2v6iktsjsrjep";
const char* mqtt_server = "test.mosquitto.org";
const int   mqtt_port   = 1883;

WiFiClient   espClient;
PubSubClient client(espClient);

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
const long publishInterval    = 5000;
const long staleCheckInterval = 5000;
const long nodeTimeoutMs      = 30000;   // mark OFFLINE if silent this long

// =====================================================
// Dynamic Binding Registry (in-memory, 3 slots)
// =====================================================
struct BindingEntry {
  String slot;            // "field_a" / "field_b" / "field_c"
  String boundUid;        // empty if unbound
  bool   online;          // last-known status
  unsigned long lastSeen; // millis() of last received packet
  // Cached latest readings (populated as packets arrive)
  float temperature;
  float humidity;
  float moisture;
  bool  hasData;
};

const int NUM_SLOTS = 3;
BindingEntry slots[NUM_SLOTS] = {
  { "field_a", "", false, 0, 0, 0, 0, false },
  { "field_b", "", false, 0, 0, 0, 0, false },
  { "field_c", "", false, 0, 0, 0, 0, false }
};

// Pending unknown UIDs we've already announced (so we don't spam new-node messages)
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
// Registry helpers
// =====================================================
int findSlotByUid(const String& uid) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (slots[i].boundUid == uid && uid.length() > 0) return i;
  }
  return -1;
}

int findSlotByName(const String& name) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (slots[i].slot == name) return i;
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
  if (announcedCount < MAX_PENDING) {
    announcedUids[announcedCount++] = uid;
  }
}

// =====================================================
// WiFi Connect
// =====================================================
void connectWiFi() {
  showMessage("WiFi Connecting", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    retry++;
    showMessage("WiFi Connecting", "Please wait...", "Retry: " + String(retry));
    if (retry > 40) {
      showMessage("WiFi Failed", "Restarting...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  showMessage("WiFi Connected", WiFi.localIP().toString());
  delay(1000);
}

// =====================================================
// Send LoRa command to a remote node (by UID)
// =====================================================
void sendLoRaCommand(const String& uid, const String& cmd) {
  String packet = "UID=" + uid + ",CMD=" + cmd;
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("LoRa TX: " + packet);
  showMessage("LoRa TX", uid, cmd);
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

  // ----- Local valve control -----
  if (topicStr == "field_main/control") {
    if (message == "ON" || message == "OPEN") {
      digitalWrite(VALVE_CONTROL_PIN, HIGH);
    } else if (message == "OFF" || message == "CLOSE") {
      digitalWrite(VALVE_CONTROL_PIN, LOW);
    }
    showMessage("field_main CMD", message, "Local valve");
    return;
  }

  // ----- Remote field valve commands -> forwarded over LoRa to bound UID -----
  if (topicStr == "field_a/control" || topicStr == "field_b/control" || topicStr == "field_c/control") {
    String slotName = topicStr.substring(0, topicStr.indexOf('/'));
    int idx = findSlotByName(slotName);
    if (idx >= 0 && slots[idx].boundUid.length() > 0) {
      sendLoRaCommand(slots[idx].boundUid, message);
    } else {
      Serial.println("No UID bound to " + slotName + " yet, dropping command");
    }
    return;
  }

  // ----- Binding command from dashboard -----
  if (topicStr == "gateway/command/bind") {
    // Expected payload: {"uid":"NODE-XYZ","slot":"field_b"}
    int uidStart = message.indexOf("\"uid\":\"") + 7;
    int uidEnd   = message.indexOf("\"", uidStart);
    int slotStart = message.indexOf("\"slot\":\"") + 8;
    int slotEnd   = message.indexOf("\"", slotStart);

    if (uidStart < 7 || slotStart < 8) {
      Serial.println("Malformed bind command");
      return;
    }
    String uid  = message.substring(uidStart, uidEnd);
    String slot = message.substring(slotStart, slotEnd);

    int idx = findSlotByName(slot);
    if (idx >= 0) {
      slots[idx].boundUid = uid;
      slots[idx].lastSeen = millis();
      slots[idx].online = true;
      Serial.println("BIND: " + uid + " -> " + slot);
      showMessage("Bind OK", uid, "-> " + slot);
    } else {
      Serial.println("Unknown slot: " + slot);
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
    String clientId = "Heltec_LoRa_Gateway_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("MQTT connected");
      client.subscribe("field_main/control");
      client.subscribe("field_a/control");
      client.subscribe("field_b/control");
      client.subscribe("field_c/control");
      client.subscribe("gateway/command/bind");
      showMessage("MQTT Connected", "Subscribed OK");
      delay(1000);
    } else {
      showMessage("MQTT Failed", "rc=" + String(client.state()), "Retrying...");
      delay(2000);
    }
  }
}

// =====================================================
// OLED Init
// =====================================================
void initOLED() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(50);
  digitalWrite(OLED_RST, HIGH); delay(50);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  showMessage("Heltec Booting", "OLED OK");
  delay(1000);
}

// =====================================================
// LoRa Init
// =====================================================
void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_BAND)) {
    showMessage("LoRa Failed", "Check frequency", "Check antenna");
    while (true) {}
  }
  showMessage("LoRa Ready", "Frequency OK");
  delay(1000);
}

// =====================================================
// Publish field_main sensors (with topic names matching the dashboard)
// =====================================================
void readAndPublishFieldMainSensors() {
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (isnan(humidity) || isnan(temperature)) {
    showMessage("DHT11 Error", "Check wiring", "Data pin: GPIO13");
    return;
  }

  int lightRaw    = analogRead(LIGHT_SENSOR_PIN);
  int moistureRaw = analogRead(MOISTURE_SENSOR_PIN);
  int rainRaw     = analogRead(RAIN_SENSOR_PIN);
  int valveFb     = digitalRead(VALVE_FEEDBACK_PIN);

  float lightPercent    = constrain(map(lightRaw,    0, 4095,   0, 100), 0, 100);
  float moisturePercent = constrain(map(moistureRaw, 0, 4095, 100,   0), 0, 100);
  float rainPercent     = constrain(map(rainRaw,     0, 4095, 100,   0), 0, 100);

  // Topics renamed to match the dashboard's expectations
  client.publish("field_main/temperature", String(temperature, 1).c_str());
  client.publish("field_main/humidity",    String(humidity, 1).c_str());
  client.publish("field_main/light",       String(lightPercent, 1).c_str());
  client.publish("field_main/moisture",    String(moisturePercent, 1).c_str());
  client.publish("field_main/rain",        String(rainPercent, 1).c_str());
  client.publish("field_main/valve",       valveFb ? "OPEN" : "CLOSE");
}

// =====================================================
// Parse one KEY=VALUE pair into the right field of an entry
// =====================================================
void applyPair(BindingEntry& e, const String& key, const String& value) {
  if      (key == "TEMP")  e.temperature = value.toFloat();
  else if (key == "HUM")   e.humidity    = value.toFloat();
  else if (key == "MOIST") e.moisture    = value.toFloat();
}

// =====================================================
// Handle a LoRa packet with new UID-based format:
//   UID=NODE-A1B2C3,TEMP=26.5,HUM=70.0,MOIST=55
// =====================================================
void handleLoRaPacket() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String received = "";
  while (LoRa.available()) received += (char)LoRa.read();
  received.trim();

  int rssi = LoRa.packetRssi();
  Serial.println("LoRa RX: " + received + "  RSSI=" + String(rssi));

  // Split on commas
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

  if (uid.length() == 0) {
    Serial.println("Packet missing UID, ignored");
    return;
  }

  int slotIdx = findSlotByUid(uid);

  if (slotIdx == -1) {
    // Unknown UID -> announce once for the binding dashboard to pick up
    if (!alreadyAnnounced(uid)) {
      rememberAnnounced(uid);
      String payload = "{\"uid\":\"" + uid + "\"}";
      client.publish("gateway/nodes/new", payload.c_str());
      Serial.println("ANNOUNCED new UID: " + uid);
      showMessage("New node", uid, "Awaiting bind");
    }
    return;
  }

  // Known UID -> cache values, update lastSeen, publish to slot topics
  BindingEntry& e = slots[slotIdx];
  e.lastSeen = millis();
  if (!e.online) {
    e.online = true;
    String statusPayload = "{\"node\":\"" + e.slot + "\",\"status\":\"ONLINE\"}";
    client.publish("gateway/node_status", statusPayload.c_str());
  }
  e.hasData = true;

  for (int i = 0; i < pairCount; i++) {
    int eq = pairs[i].indexOf('=');
    String key = pairs[i].substring(0, eq);
    String val = pairs[i].substring(eq + 1);
    applyPair(e, key, val);
  }

  // Publish parsed values to per-slot topics matching the dashboard
  client.publish((e.slot + "/temperature").c_str(), String(e.temperature, 1).c_str());
  client.publish((e.slot + "/humidity").c_str(),    String(e.humidity, 1).c_str());
  client.publish((e.slot + "/moisture").c_str(),    String(e.moisture, 1).c_str());
  client.publish((e.slot + "/rssi").c_str(),        String(rssi).c_str());

  showMessage("LoRa RX " + e.slot, "T=" + String(e.temperature, 1),
              "M=" + String(e.moisture, 0), "RSSI=" + String(rssi));
}

// =====================================================
// Periodically check for nodes that have gone silent
// =====================================================
void checkStaleNodes() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_SLOTS; i++) {
    BindingEntry& e = slots[i];
    if (e.boundUid.length() == 0) continue;       // unbound slots aren't expected to be online
    if (!e.online) continue;                       // already offline, no change to report

    if (now - e.lastSeen > nodeTimeoutMs) {
      e.online = false;
      String statusPayload = "{\"node\":\"" + e.slot + "\",\"status\":\"OFFLINE\"}";
      client.publish("gateway/node_status", statusPayload.c_str());
      Serial.println("STALE: " + e.slot + " (" + e.boundUid + ") marked OFFLINE");
    }
  }
}

// =====================================================
// Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(VALVE_CONTROL_PIN, OUTPUT);
  digitalWrite(VALVE_CONTROL_PIN, LOW);
  pinMode(VALVE_FEEDBACK_PIN, INPUT);

  dht.begin();
  analogReadResolution(12);

  initOLED();
  showMessage("System Starting", "field_main local", "3 LoRa slots");
  initLoRa();
  connectWiFi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  showMessage("System Ready", "LoRa + MQTT OK", "Awaiting nodes");
  Serial.println("System Ready");
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
    readAndPublishFieldMainSensors();
  }
  if (now - lastStaleCheck >= staleCheckInterval) {
    lastStaleCheck = now;
    checkStaleNodes();
  }
}
