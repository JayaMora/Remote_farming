#include <DHT.h>
#include <SPI.h>
#include <RH_RF95.h>

// --- Sensor Configuration ---
#define DHTPIN 2          // Digital pin for DHT11
#define DHTTYPE DHT11     // DHT 11
#define SOIL_PIN A0       // Analog pin for Soil Moisture


// --- LoRa (RFM95) Configuration ---
#define RFM95_CS   8
#define RFM95_RST  4   // Make sure the RST wire is plugged into Digital 4!
#define RFM95_INT  3   // CHANGED FROM 7 TO 3 (Must be a hardware interrupt pin)
#define RF95_FREQ 915.0

// --- Timing ---
#define SEND_INTERVAL 10000UL   // 10 seconds, non-blocking

// --- Soil Calibration (REPLACE with your measured values — see calibration steps) ---
#define SOIL_AIR_VALUE   590    // raw ADC reading in dry air
#define SOIL_WATER_VALUE 280    // raw ADC reading fully submerged in water

DHT dht(DHTPIN, DHTTYPE);
RH_RF95 rf95(RFM95_CS, RFM95_INT);

unsigned long lastSendTime = 0;

void setup() {
  Serial.begin(9600);
  while (!Serial) delay(1); // Needed on some boards; harmless on Uno

  dht.begin();

  // Reset the RFM95 module
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  Serial.println("--- LoRa Sensor Transmitter Initialized ---");

  if (!rf95.init()) {
    Serial.println("ERROR: RFM95 init failed. Check wiring.");
    while (1); // Halt — no point continuing without radio
  }

  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("ERROR: setFrequency failed.");
    while (1);
  }

  rf95.setTxPower(20, false);  // Max power for RFM95 (range 5-23 dBm)

  Serial.println("RFM95 initialized successfully.");
  Serial.println("Format: Temperature(C), Humidity(%), SoilMoisture(%)");
  Serial.println("---------------------------------------------------");
}

void loop() {
  unsigned long now = millis();

  // Non-blocking 10-second interval (avoids using delay() so radio stays responsive)
  if (now - lastSendTime < SEND_INTERVAL) {
    return;
  }
  lastSendTime = now;

  // 1. Read DHT11 (Temperature & Humidity)
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Error: Could not read from DHT11 sensor. Skipping this cycle.");
    return; // Skip this cycle, try again in 10s
  }

  // 2. Read Soil Moisture (calibrated)
  int rawSoilValue = analogRead(SOIL_PIN);
  int soilMoisturePercent = map(rawSoilValue, SOIL_AIR_VALUE, SOIL_WATER_VALUE, 0, 100);
  soilMoisturePercent = constrain(soilMoisturePercent, 0, 100);

  // 3. Construct the Data Packet (Comma-Separated Values)
  String packet = String(temperature) + "," + String(humidity) + "," + String(soilMoisturePercent);

  // 4. Display locally for debugging
  Serial.print("Sending: ");
  Serial.println(packet);

  // 5. Transmit over LoRa
  // RadioHead needs a uint8_t buffer, not a String directly
  char radioPacket[32];
  packet.toCharArray(radioPacket, sizeof(radioPacket));

  rf95.send((uint8_t *)radioPacket, strlen(radioPacket));
  rf95.waitPacketSent();  // Blocks briefly until transmission completes

  Serial.println("Packet sent over LoRa.");
}
