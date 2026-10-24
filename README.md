# Waveshare ESP32-S3 Relay 6CH

## Local configuration

Copy `src/secrets.example.h` to `src/secrets.h` and enter your Wi-Fi name,
Wi-Fi password, and MQTT server. `src/secrets.h` is ignored by Git, so local
credentials are not included in new commits.

Build the firmware with PlatformIO after creating `src/secrets.h`.

## Source layout

- `src/main.cpp`: startup order and main loop
- `src/bridge.cpp`: TCP ports, RS485 forwarding, command queue, and debug records
- `src/mqtt.cpp`: MQTT connection, topics, and relay commands
- `src/network.cpp`: Wi-Fi connection and OTA
- `src/relays.cpp`: relay pins and switching
- `src/led.cpp`: status LED
- `src/config.cpp`: includes the local `secrets.h` once for the whole firmware
