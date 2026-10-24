#include "mqtt.h"
#include "bridge.h"
#include "led.h"
#include "relays.h"
#include "config.h"
#include <WiFi.h>

namespace {
const char* MQTT_RS485_INFO_TOPIC = "waveshare/rs485";
const char* MQTT_DEVICE_TOPIC = "waveshare/device";
const char* MQTT_DEVICE_STATE_TOPIC = "waveshare/device/state";
const uint16_t MQTT_PORT = 1883;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
unsigned long lastMqttAttemptMillis = 0;
WiFiClient wifiClient;
}

const char* MQTT_CLIENT_ID = "esp32s3-relay-6ch";
PubSubClient mqttClient(wifiClient);

void publishRelayState(int index) {
  char topic[48];
  char payload[8];
  snprintf(topic, sizeof(topic), "waveshare/relay/%d/state", index + 1);
  snprintf(payload, sizeof(payload), "%s", relayIsOn(index) ? "ON" : "OFF");
  mqttClient.publish(topic, payload, true);
}

void publishDeviceState(const char* state) {
  mqttClient.publish(MQTT_DEVICE_STATE_TOPIC, state, true);
}

void clearLegacyDeviceTopics() {
  mqttClient.publish("waveshare/device/info", "", true);
  mqttClient.publish("waveshare/device/ip", "", true);
}

void publishDeviceTopic() {
  char payload[160];
  snprintf(
    payload,
    sizeof(payload),
    "{\"controller\":\"Waveshare ESP32 6CH\",\"relays\":6,\"ip\":\"%s\"}",
    WiFi.localIP().toString().c_str()
  );
  mqttClient.publish(MQTT_DEVICE_TOPIC, payload, true);
}

void publishRs485Topic() {
  char payload[96];
  snprintf(
    payload,
    sizeof(payload),
    "{\"tcp_controle_socket\":%u,\"tcp_sniffer_socket\":%u,\"tcp_debug_socket\":%u}",
    TCP_PORT,
    TCP_SNIFFER_PORT,
    TCP_DEBUG_PORT
  );
  mqttClient.publish(MQTT_RS485_INFO_TOPIC, payload, true);
}

void publishAllStates() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    publishRelayState(i);
  }
}

bool clearRetainedRelayCommands() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    char topic[48];
    snprintf(topic, sizeof(topic), "waveshare/relay/%d/set", i + 1);
    if (!mqttClient.publish(topic, "", true)) {
      return false;
    }
  }
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);

  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();

  Serial.printf("MQTT empfangen: %s -> %s\n", topic, message.c_str());

  if (topicStr.startsWith("waveshare/relay/") && topicStr.endsWith("/set")) {
    int startPos = topicStr.indexOf("waveshare/relay/") + strlen("waveshare/relay/");
    int endPos = topicStr.lastIndexOf('/');
    if (startPos >= 0 && endPos > startPos) {
      String relayPart = topicStr.substring(startPos, endPos);
      int relayIndex = relayPart.toInt() - 1;
      if (relayIndex >= 0 && relayIndex < RELAY_COUNT) {
        bool on = message.equalsIgnoreCase("ON") || message.equals("1") || message.equalsIgnoreCase("TRUE");
        bool off = message.equalsIgnoreCase("OFF") || message.equals("0") || message.equalsIgnoreCase("FALSE");
        if (on) {
          setRelay(relayIndex, true);
        } else if (off) {
          setRelay(relayIndex, false);
        } else {
          Serial.println("Ungültige MQTT-Nachricht, verwende ON/OFF oder 1/0.");
        }
      } else {
        Serial.printf("Ungültiger Relaisindex: %s\n", relayPart.c_str());
      }
    }
  }
}

bool connectMQTT(bool force) {
  if (mqttClient.connected()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  unsigned long now = millis();
  if (!force && lastMqttAttemptMillis != 0 && now - lastMqttAttemptMillis < MQTT_RETRY_INTERVAL) {
    return false;
  }
  lastMqttAttemptMillis = now;

  Serial.print("Verbinde mit MQTT... ");
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(15);
  mqttClient.setSocketTimeout(3);
  wifiClient.stop();

  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_DEVICE_STATE_TOPIC, 1, true, "offline")) {
    setRgbColor(0, 0, 64); // Blau = MQTT verbunden
    Serial.println("MQTT verbunden");
    // Alte Retain-Befehle vor der Subscription entfernen, damit sie keine Relais schalten.
    if (!clearRetainedRelayCommands()) {
      mqttClient.disconnect();
      return false;
    }
    mqttClient.subscribe("waveshare/relay/+/set");
    clearLegacyDeviceTopics();
    publishDeviceState("online");
    publishDeviceTopic();
    publishRs485Topic();
    publishAllStates();
    return true;
  }

  lastMqttAttemptMillis = millis();
  Serial.printf("fehlgeschlagen, rc=%d\n", mqttClient.state());
  return false;
}

void resetMqttRetry() {
  lastMqttAttemptMillis = 0;
}

void stopMqttClient() {
  mqttClient.disconnect();
  wifiClient.stop();
}
