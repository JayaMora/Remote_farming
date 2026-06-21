#include <SPI.h>
#include <LoRa.h>

// Define the SPI pins for the LoRa module
// These match the wiring table above
#define ss 10
#define rst 9
#define dio0 2

void setup() {
  // Initialize the Serial Monitor
  Serial.begin(115200);
  while (!Serial); // Wait for Serial to initialize

  Serial.println("===== Arduino LoRa Sender =====");

  // Configure the LoRa pins
  LoRa.setPins(ss, rst, dio0);

  // Initialize LoRa at 915 MHz
  if (!LoRa.begin(915E6)) {
    Serial.println("Starting LoRa failed! Check your wiring.");
    while (1); // Halt execution if LoRa fails
  }
  
  // Optional: You can configure the sync word to match the receiver
  // if the receiving code uses one (e.g., LoRa.setSyncWord(0xF3);)
  
  Serial.println("LoRa Initialized Successfully at 915 MHz!");
  Serial.println("Type a message in the Serial Monitor and press Enter to send.");
  Serial.println("===============================");
}

void loop() {
  // Check if you have typed anything into the Serial Monitor
  if (Serial.available()) {
    // Read the serial input until a newline character
    String outgoingMessage = Serial.readStringUntil('\n'); 
    outgoingMessage.trim(); // Clean up trailing spaces or hidden characters (\r)

    // Only send if the string isn't empty
    if (outgoingMessage.length() > 0) {
      Serial.print("Transmitting: ");
      Serial.println(outgoingMessage);

      // Begin LoRa packet, write the serial data, and close the packet
      LoRa.beginPacket();
      LoRa.print(outgoingMessage);
      LoRa.endPacket();

      Serial.println("Transmission complete.");
    }
  }
}