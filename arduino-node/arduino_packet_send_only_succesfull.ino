#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>

// LoRa Pins
#define ss 10
#define rst 9
#define dio0 2

// Sensor Pins
#define DHTPIN 4
#define DHTTYPE DHT11 // Change to DHT22 if you are using a DHT22 sensor
#define SOIL_PIN A0

// Initialize DHT sensor
DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("===== LoRa Sensor Sender =====");

  // Initialize DHT sensor
  dht.begin();

  // Configure LoRa pins
  LoRa.setPins(ss, rst, dio0);

  // Initialize LoRa at 915 MHz
  if (!LoRa.begin(915E6)) {
    Serial.println("Starting LoRa failed! Check your wiring.");
    while (1); 
  }
  
  // Set transmission power to maximum as verified in previous step
  LoRa.setTxPower(20); 
  
  Serial.println("LoRa Initialized Successfully at 915 MHz!");
  Serial.println("====================================");
}

void loop() {
  // Read values from DHT11/DHT22
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature(); // Temperature in Celsius

  // Read value from Soil Moisture Sensor (returns 0 to 1023)
  int soilMoisture = analogRead(SOIL_PIN);

  // Check if any reads failed and exit early (to try again)
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT sensor!");
    delay(2000);
    return;
  }

  // Create a packed data string comma-separated format
  // Example payload format: "H:60.0,T:24.5,S:450"
  String dataPacket = "H:" + String(humidity, 1) + 
                      ",T:" + String(temperature, 1) + 
                      ",S:" + String(soilMoisture);

  Serial.print("Sending Packet: ");
  Serial.println(dataPacket);

  // Transmit the packed string over LoRa
  LoRa.beginPacket();
  LoRa.print(dataPacket);
  
  if (LoRa.endPacket() == 1) {
    Serial.println("Status: Success");
  } else {
    Serial.println("Status: Failed");
  }
  Serial.println("-------------------------");

  // Wait 5 seconds before the next reading/transmission
  delay(5000); 
}