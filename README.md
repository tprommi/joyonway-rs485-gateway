# Joyonway RS485 gateway

Firmware for the **Waveshare Industrial 6CH ESP32-S3 WiFi Relay Module** used with pools that have a Joyonway controller. It connects the controller's RS485 bus to a TCP client such as `ha-joyonway` and exposes the board's six relays through MQTT.

<a href="https://www.waveshare.com/media/catalog/product/e/s/esp32-s3-relay-6ch-1.jpg"><img src="https://www.waveshare.com/media/catalog/product/e/s/esp32-s3-relay-6ch-1.jpg" alt="Waveshare Industrial 6CH ESP32-S3 WiFi Relay Module" width="320"></a>

Hardware: [Waveshare ESP32-S3-Relay-6CH](https://www.waveshare.com/product/modules/esp32-s3-relay-6ch.htm). Budget approximately **€50** for the module, depending on seller and shipping; check the current price before ordering. Image: Waveshare.

For weak Wi-Fi reception, a [2.4 GHz Yagi antenna](https://de.aliexpress.com/item/1005011837962454.html) may help. A Yagi focuses both transmission and reception toward the access point, so it should be aimed accordingly. The board has an SMA female antenna connector; check that the antenna or its cable has a matching SMA male connector before ordering.

## How it works

The gateway forwards bytes received from RS485 to the TCP control client. Commands sent by that client are assembled into complete Joyonway frames (`0x1A ... 0x1D`) and queued. The gateway watches the controller's bus traffic and transmits one queued command after a sync frame and the following `0x21` poll, with a 10 ms guard interval. This lets the controller's traffic determine when commands are inserted; TCP input is not written to RS485 immediately. The queue holds up to eight frames, and commands expire after five seconds. It is cleared when the control client or Wi-Fi connection closes.

| TCP port | Purpose |
| --- | --- |
| `8080` | Control connection: raw RS485 receive stream to the client; accepts Joyonway command frames from the client for scheduled RS485 transmission. |
| `8081` | Read-only raw sniffer stream containing received and transmitted bytes, without direction markers. |
| `8082` | Read-only binary debug stream with timestamps and direction/event information. |

Each port accepts one client at a time; a new connection replaces the previous client on that port. See [the TCP bridge documentation](src/tcp_bridge.md) for the timing details, debug format, and capture tool.

## MQTT

The gateway connects to the broker configured in `src/secrets.h` on port `1883`. It uses client ID `esp32s3-relay-6ch` and the following topics:

| Topic | Direction | Payload | Retained |
| --- | --- | --- | --- |
| `waveshare/device/state` | Gateway → broker | `online` or `offline` | Yes; `offline` is the MQTT last will. |
| `waveshare/device` | Gateway → broker | JSON with `controller`, `relays`, and `ip` | Yes |
| `waveshare/rs485` | Gateway → broker | JSON with `tcp_controle_socket`, `tcp_sniffer_socket`, and `tcp_debug_socket` | Yes |
| `waveshare/relay/N/state` | Gateway → broker | `ON` or `OFF`, where `N` is `1` through `6` | Yes |
| `waveshare/relay/N/set` | Broker → gateway | `ON`/`OFF`, `1`/`0`, or `TRUE`/`FALSE` (case insensitive for words) | Do not retain commands. |

For example, publish `ON` to `waveshare/relay/1/set` to turn on relay 1. Its reported state appears on `waveshare/relay/1/state`. The relays start in the off state when the firmware boots. On each MQTT connection, the gateway clears retained `/set` messages before subscribing, then publishes the device information and all relay states.

Example information payloads (the IP is the gateway's current Wi-Fi address):

```json
{"controller":"Waveshare ESP32 6CH","relays":6,"ip":"192.168.1.50"}
```

```json
{"tcp_controle_socket":8080,"tcp_sniffer_socket":8081,"tcp_debug_socket":8082}
```

## Configuration and upload

Copy `src/secrets.example.h` to `src/secrets.h` and enter your Wi-Fi name, Wi-Fi password, and MQTT broker hostname or IP. `src/secrets.h` is ignored by Git. The firmware currently connects to MQTT without a username or password.

Build and upload with PlatformIO. `platformio.ini` offers `esp32s3_devkit_tty` for a USB/serial upload and `esp32s3_devkitc_ota` for Wi-Fi OTA; OTA is the default environment. For the first upload, use the serial environment:

```shell
pio run -e esp32s3_devkit_tty -t upload
```

After the device has joined Wi-Fi, OTA targets `esp32s3-relay-6ch.local`:

```shell
pio run -e esp32s3_devkitc_ota -t upload
```

If your computer cannot resolve that hostname, pass the gateway's current IP with `--upload-port <ip-address>`. The IP is printed in the serial monitor and published in `waveshare/device`.

## Source layout

- `src/bridge.cpp`: TCP ports, RS485 forwarding, command queue, and debug records
- `src/mqtt.cpp`: MQTT connection, topics, and relay commands
- `src/network.cpp`: Wi-Fi connection and OTA
- `src/relays.cpp`: relay pins and switching
- `src/main.cpp`: startup and main loop
