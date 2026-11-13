// ECSE3038 - Mock Test 2 Firmware
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <PubSubClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#define X_API_KEY "default_key"
#define MQTT_NAME "qwerty2"
#endif

const char *API_URL = "http://iot-xackary.belgiumcentral.cloudapp.azure.com";

const char *BROKER_HOST = "135.232.203.144";
const uint16_t BROKER_PORT = 1883;

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_EVERY_MS = 5000;
const int DHT_PIN = 15;
const unsigned long SEND_EVERY_MS = 10000;
const int LED_PIN = 2;

DHTesp dht;
WiFiClient net;
PubSubClient mqtt(net);

String statusTopic;
String alarmMaxTopic;
String alarmStateTopic;

unsigned long lastSend = 0;
unsigned long lastAttempt = 0;
float tempLimit = NAN;
float lastTemp = NAN;
String reportedLed = "";

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Trying again in 5 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  return true;
}

void sendReading(float temp) {
  char body[80];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"temp\": %.1f}",
           WiFi.macAddress().c_str(), temp);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", X_API_KEY);
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  
  if (status == 401) {
    Serial.println("Unauthorized: Check your X_API_KEY in include/secrets.h.");
  } else if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /devices first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

bool parseLimit(const String &message, float &limit) {
  if (message.length() == 0) {
    return false;
  }
  char *end;
  float value = strtof(message.c_str(), &end);
  if (*end != '\0') {
    return false;
  }
  limit = value;
  return true;
}

void updateLed() {
  bool tooHot = false;
  if (!isnan(tempLimit) && !isnan(lastTemp) && lastTemp > tempLimit) {
    tooHot = true;
  }

  String led = tooHot ? "on" : "off";
  digitalWrite(LED_PIN, tooHot ? HIGH : LOW);

  Serial.print("Limit: ");
  Serial.print(tempLimit);
  Serial.print("  Temperature: ");
  Serial.print(lastTemp);
  Serial.print("  -> LED ");
  Serial.println(led);

  if (led != reportedLed && mqtt.connected()) {
    mqtt.publish(alarmStateTopic.c_str(), led.c_str(), true);
    reportedLed = led;
  }
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Got ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  String t = topic;
  if (t == alarmMaxTopic) {
    float newLimit;
    if (parseLimit(message, newLimit)) {
      tempLimit = newLimit;
      updateLed();
    } else {
      Serial.println("Not a number: limit unchanged.");
    }
  }
}

bool connectMqtt() {
  String clientId = "esp32-" + String(MQTT_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to broker ");
  Serial.print(BROKER_HOST);
  Serial.print(" as ");
  Serial.println(clientId);

  bool ok = mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline");   
  
  if (!ok) {
    Serial.print("Broker said no, state ");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("Connected to broker");

  mqtt.publish(statusTopic.c_str(), "online", true);
  mqtt.subscribe(alarmMaxTopic.c_str());
  reportedLed = "";
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello from the ESP32");
  dht.setup(DHT_PIN, DHTesp::DHT22);
  pinMode(LED_PIN, OUTPUT);

  String base = "ecse3038/" + String(MQTT_NAME);
  statusTopic = base + "/status";
  alarmMaxTopic = base + "/alarm/max";
  alarmStateTopic = base + "/alarm/state";

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastAttempt >= RETRY_EVERY_MS) {
      lastAttempt = millis();
      connectWiFi();
    }
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
  } else if (millis() - lastAttempt >= RETRY_EVERY_MS) {
    lastAttempt = millis();
    connectMqtt();
  }

  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  float temp = dht.getTemperature();
  if (isnan(temp)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  lastTemp = temp;
  sendReading(temp);
  updateLed();
}