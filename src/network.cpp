#include "network.h"
#include "bridge.h"
#include "led.h"
#include "mqtt.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

namespace {
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;
unsigned long lastWiFiAttemptMillis = 0;
bool networkClientsStopped = false;
}

bool networkClientsAreStopped() {
  return networkClientsStopped;
}

void stopNetworkClients() {
  stopBridgeClients();
  stopMqttClient();
  networkClientsStopped = true;
}

bool connectWiFi(bool force) {
  if (WiFi.status() == WL_CONNECTED) {
    networkClientsStopped = false;
    return true;
  }

  unsigned long now = millis();
  if (!force && lastWiFiAttemptMillis != 0 && now - lastWiFiAttemptMillis < WIFI_RETRY_INTERVAL) {
    return false;
  }
  lastWiFiAttemptMillis = now;

  Serial.printf("Verbinde mit WLAN '%s'...\n", WIFI_SSID);
  setRgbColor(64, 0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    lastWiFiAttemptMillis = millis();
    Serial.printf("WLAN-Verbindung fehlgeschlagen, status=%d. Neuer Versuch in %lu ms.\n", WiFi.status(), WIFI_RETRY_INTERVAL);
    WiFi.disconnect(false);
    return false;
  }

  resetMqttRetry();
  networkClientsStopped = false;
  setRgbColor(0, 64, 0); // Grün = WLAN steht
  Serial.printf("WLAN verbunden, IP: %s, RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void setupOTA() {
  ArduinoOTA.setHostname(MQTT_CLIENT_ID);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA Update gestartet");
    setRgbColor(64, 32, 0);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update beendet");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Fortschritt: %u%%\r", (progress * 100) / total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Fehler[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth fehlgeschlagen");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin fehlgeschlagen");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect fehlgeschlagen");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive fehlgeschlagen");
    else if (error == OTA_END_ERROR) Serial.println("End fehlgeschlagen");
  });

  ArduinoOTA.begin();
  Serial.printf("OTA bereit: %s.local / %s\n", MQTT_CLIENT_ID, WiFi.localIP().toString().c_str());
}
