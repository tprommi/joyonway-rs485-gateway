#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "bridge.h"
#include "led.h"
#include "mqtt.h"
#include "network.h"
#include "relays.h"

namespace {
unsigned long lastHeartbeatMillis = 0;
const unsigned long HEARTBEAT_INTERVAL = 2000;
}

void setup() {
  Serial.begin(115200);
  
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {
    delay(10);
  }
  
  Serial.println("\n--- Serial gestartet auf 115200 ---");

  initLed();

  initRs485();

  initRelays();

  // 1. ZUERST WLAN aufbauen, um den Netzwerk-Stack sauber zu initialisieren
  connectWiFi();

  // 2. JETZT den TCP-Server sicher starten (verhindert den "Invalid mbox" Crash)
  startBridgeServers();
  setupOTA();
  Serial.println("Waveshare ESP32-S3 Relay 6CH - Start");

  // 3. Als letztes MQTT verbinden
  connectMQTT(true);

  Serial.println("Relais initialisiert. Sende 1-6 über Serial zum Schalten.");
}

void loop() {
  // WLAN-Verbindung überwachen und ggf. wiederherstellen
  if (WiFi.status() != WL_CONNECTED) {
    if (!networkClientsAreStopped()) {
      Serial.println("WLAN getrennt, schliesse alte TCP/MQTT-Verbindungen.");
      stopNetworkClients();
    }
    connectWiFi();
    ArduinoOTA.handle();
    return;
  }

  // MQTT-Verbindung überwachen und ggf. wiederherstellen
  if (!mqttClient.connected()) {
    connectMQTT();
  }

  // MQTT-Prozesse im Hintergrund am Leben erhalten
  if (mqttClient.connected()) {
    mqttClient.loop();
  }
  ArduinoOTA.handle();

  pollBridge();

  // Status-LED aktualisieren
  unsigned long now = millis();
  if (now - lastHeartbeatMillis >= HEARTBEAT_INTERVAL) {
    lastHeartbeatMillis = now;
    
    bool wifiOk = WiFi.status() == WL_CONNECTED;
    bool mqttOk = mqttClient.connected();
    
    // LED-Farbsteuerung je nach Verbindungsstatus
    if (mqttOk) {
      setRgbColor(0, 0, 64); // Blau = Alles läuft perfekt (WLAN + MQTT)
    } else if (wifiOk) {
      setRgbColor(0, 64, 0); // Grün = Nur WLAN verbunden
    } else {
      setRgbColor(64, 0, 0); // Rot = Offline
    }
    
  }
}
