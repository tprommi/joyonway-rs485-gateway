#include "relays.h"
#include "mqtt.h"
#include <Arduino.h>

namespace {
const int relayPins[RELAY_COUNT] = {1, 2, 41, 42, 45, 46};
}

void initRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
}

bool relayIsOn(int index) {
  return digitalRead(relayPins[index]) == HIGH;
}

void setRelay(int index, bool on) {
  digitalWrite(relayPins[index], on ? HIGH : LOW);
  publishRelayState(index);
  Serial.printf("Relais %d ist jetzt %s\n", index + 1, on ? "AN" : "AUS");
}

void toggleRelay(int index) {
  bool current = digitalRead(relayPins[index]) == HIGH;
  setRelay(index, !current);
}
