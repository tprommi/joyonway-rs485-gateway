#pragma once
#include <PubSubClient.h>

extern PubSubClient mqttClient;
extern const char* MQTT_CLIENT_ID;

bool connectMQTT(bool force = false);
void resetMqttRetry();
void stopMqttClient();
void publishRelayState(int index);
