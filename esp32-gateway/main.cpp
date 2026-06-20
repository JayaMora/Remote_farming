#include <WiFi.h>
#include <PubSubClient.h>

// ---------------- WiFi ----------------
const char* ssid = "Galaxy A56 5G E79B";
const char* password = "jw2v6iktsjsrjep";

// ---------------- MQTT ----------------
const char* mqtt_server = "test.mosquitto.org"; 
// 👉 Replace with your PC IP running Mosquitto

WiFiClient espClient;
PubSubClient client(espClient);

// ---------------- LED ----------------
#define LED_PIN 2

// ---------------- Callback (Receive messages) ----------------
void callback(char* topic, byte* payload, unsigned int length)
{
    String message = "";

    for (int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    Serial.print("Message received: ");
    Serial.println(message);

    if (message == "ON")
    {
        digitalWrite(LED_PIN, HIGH);
    }
    else if (message == "OFF")
    {
        digitalWrite(LED_PIN, LOW);
    }
}

// ---------------- Reconnect MQTT ----------------
void reconnect()
{
    while (!client.connected())
    {
        Serial.print("Connecting to MQTT...");

        if (client.connect("ESP32_Client"))
        {
            Serial.println("connected");

            // Subscribe to control topic
            client.subscribe("field_a/control");
            client.subscribe("field_b/control");
            client.subscribe("field_c/control");
            
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" retrying in 2 seconds");

            delay(2000);
        }
    }
}

// ---------------- Setup ----------------
void setup()
{
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);

    // WiFi connect
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWiFi connected");

    // MQTT setup
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
}

// ---------------- Loop ----------------
void loop()
{
    if (!client.connected())
    {
        reconnect();
    }

    client.loop();

    // Simulated sensor data (replace later with real sensor)
    float temp = random(250, 350) / 10.0;
    float humidity = random(400, 600) / 10.0;
    float moisture = random(200, 800) / 10.0;
    float light = random(0, 1000) / 10.0;
    float rain = random(0, 100) / 10.0;
    float valve_status = random(0, 100);

    String payload_1 = String(temp);
    String payload_2 = String(humidity);
    String payload_3 = String(moisture);
    String payload_4 = String(light);
    String payload_5 = String(rain);
    String payload_6 = String(valve_status);

    // Publish sensor data
    client.publish("field_a/temp", payload_1.c_str());
    client.publish("field_a/humidity", payload_2.c_str());
    client.publish("field_a/moisture", payload_3.c_str());
    client.publish("field_a/light", payload_4.c_str());
    client.publish("field_a/rain", payload_5.c_str());
    client.publish("field_a/valve_status", payload_6.c_str());

    Serial.println("Published: " + payload_1 + ", " + payload_2 + ", " + payload_3 + ", " + payload_4 + ", " + payload_5 + ", " + payload_6);

    delay(5000);
}