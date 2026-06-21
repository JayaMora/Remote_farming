#include <DHT.h>
#include <SoftwareSerial.h>

// --- Configuration ---
#define DHTPIN 2          
#define DHTTYPE DHT11     
#define SOIL_PIN A0       

// Create a new serial connection on digital pins 10 (RX) and 11 (TX)
SoftwareSerial linkSerial(10, 11); 
DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(9600);       // For your computer screen
  linkSerial.begin(9600);   // For talking to the other microcontroller
  dht.begin();
  
  Serial.println("Transmitter Node Started...");
}

void loop() {
  delay(2000); 

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature(); 

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Error: DHT11 failed.");
    return;
  }

  int rawSoilValue = analogRead(SOIL_PIN);
  int soilMoisturePercent = map(rawSoilValue, 1023, 0, 0, 100);
  soilMoisturePercent = constrain(soilMoisturePercent, 0, 100);

  // Construct the packet
  String packet = String(temperature) + "," + String(humidity) + "," + String(soilMoisturePercent);

  // Print to computer (for debugging)
  Serial.println("Sending: " + packet);

  // Send the packet out of Digital Pin 11 to the other microcontroller
  linkSerial.println(packet);
}