#include <DHT.h>

// --- Configuration ---
#define DHTPIN 2          // Digital pin for DHT11
#define DHTTYPE DHT11     // DHT 11
#define SOIL_PIN A0       // Analog pin for Soil Moisture

//DHT - DAT -> Digital pin 2(of arduino)
//Soil - AO ->A0 of arduino

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  // Initialize serial communication at 9600 bits per second:
  Serial.begin(9600);
  
  // Initialize the DHT11 sensor
  dht.begin();
  
  Serial.println("--- Local Sensor Test Initialized ---");
  Serial.println("Format: Temperature(C), Humidity(%), SoilMoisture(%)");
  Serial.println("---------------------------------------------------");
}

void loop() {
  // Wait 2 seconds between measurements (DHT11 updates slowly)
  delay(2000); 

  // 1. Read DHT11 (Temperature & Humidity)
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature(); 

  // Check if the DHT11 reading failed
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Error: Could not read from DHT11 sensor.");
    return; // Skip the rest of the loop and try again
  }

  // 2. Read Soil Moisture
  int rawSoilValue = analogRead(SOIL_PIN);
  
  // Map the raw analog values (0-1023) to a percentage (0-100%)
  // Adjust 1023 (bone dry) and 0 (pure water) based on your real-world calibration
  int soilMoisturePercent = map(rawSoilValue, 1023, 0, 0, 100);
  soilMoisturePercent = constrain(soilMoisturePercent, 0, 100);

  // 3. Construct the Data Packet (Comma-Separated Values)
  String packet = String(temperature) + "," + String(humidity) + "," + String(soilMoisturePercent);

  // 4. Display the packet on the Serial Monitor
  Serial.println(packet);
}