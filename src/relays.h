#pragma once

constexpr int RELAY_COUNT = 6;
void initRelays();
bool relayIsOn(int index);
void setRelay(int index, bool on);
void toggleRelay(int index);
