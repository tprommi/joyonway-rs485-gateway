#pragma once
#include <Arduino.h>

constexpr uint16_t TCP_PORT = 8080;
constexpr uint16_t TCP_SNIFFER_PORT = 8081;
constexpr uint16_t TCP_DEBUG_PORT = 8082;

void initRs485();
void startBridgeServers();
void pollBridge();
void stopBridgeClients();
