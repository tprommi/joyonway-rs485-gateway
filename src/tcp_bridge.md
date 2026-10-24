# TCP-to-RS485 bridge

- Control port `8080`: one TCP client; forwards RS485 receive bytes and queues complete Joyonway command frames for synchronized transmission.
- Sniffer port `8081`: backward-compatible raw mirror containing both RX and TX bytes without direction metadata.
- Debug port `8082`: timestamped and directional binary records for bus timing analysis.
- No RS485 DE/RE control pin is implemented; the hardware handles direction automatically.

## Debug record format

All integers use network byte order. Each record is self-delimiting:

| Field | Size | Meaning |
| :--- | ---: | :--- |
| Magic | 4 bytes | ASCII `JWDB` |
| Version | 1 byte | `1` |
| Event | 1 byte | Event type below |
| Sequence | 4 bytes | Monotonic record counter; wraps after `0xffffffff` |
| Timestamp | 8 bytes | ESP32 microseconds since boot |
| Payload length | 2 bytes | Number of following payload bytes |
| Payload | variable | Raw bytes where applicable |

Event types:

| Value | Name | Timestamp meaning | Payload |
| ---: | :--- | :--- | :--- |
| `1` | `RX_READ` | UART bytes returned to the bridge | Received bytes |
| `2` | `TX_START` | Immediately before `HardwareSerial.write()` | Command bytes |
| `3` | `TX_QUEUED` | Immediately after bytes were queued to UART | Empty |
| `4` | `TX_COMPLETE` | After `HardwareSerial.flush()` reports the UART empty | Empty |
| `5` | `COMMAND_ACCEPTED` | Complete TCP command entered the gateway queue | Command bytes |
| `6` | `SYNC_ARMED` | A local sync frame armed the 30 ms transmit timer | Empty |
| `7` | `COMMAND_DROPPED` | A queued command was rejected, expired, or cleared | Command bytes where available |
| `8` | `POLL_21_ARMED` | A complete `0x21` poll armed the 10 ms guard timer | Empty |

The three TX records are written to TCP only after `TX_COMPLETE`. Their embedded timestamps describe the UART operation and are not affected by debug-socket latency. `flush()` makes `TX_COMPLETE` an upper bound for the final RS485 stop bit leaving the ESP32 UART.

## Local command pacing

TCP command bytes are assembled into complete `0x1A ... 0x1D` Joyonway frames. Complete frames enter an eight-frame FIFO with a five-second timeout. A queued command is armed by the next locally received sync frame. The gateway then waits for the complete `0x21` poll and starts UART transmission after a 10 ms guard interval. It sends no more than one command per sync cycle. Commands that arrive after a sync wait for the next sync.

The queue is cleared whenever the control client or Wi-Fi connection closes or is replaced. This prevents a stale command from being transmitted after reconnection. Incomplete data, overlong frames, queue overflow, and expired commands are discarded rather than written to RS485.

Port `8081` remains unchanged for existing tools. Port `8082` provides enough information for RS485 protocol captures without Wireshark. The capture reader stores the lossless debug stream, reconstructs Joyonway frames, and produces timing analysis.

## Capturing and replaying

Start a live capture with:

```shell
python python/debug_read.py --host <gateway-address> --output captures/heater_test
```

Stop it with Ctrl+C. Four files are produced:

- `captures/heater_test.jwdb`: lossless binary records as received from port `8082`
- `captures/heater_test.csv`: reconstructed RX/TX frames and timing columns
- `captures/heater_test_events.csv`: every RX/TX UART event, including queue and transmit completion times
- `captures/heater_test_summary.md`: capture counters and a table of all transmitted commands

Replay or reanalyse a saved binary capture without connecting to the gateway:

```shell
python python/debug_read.py --input captures/heater_test.jwdb --output captures/heater_test_replay
```

The CSV contains the interval to the preceding frame, the preceding frame in the same direction, and the last received sync frame. RX timestamps belong to the UART read that completed the frame. When a frame spans multiple UART reads, its timestamp is therefore the final chunk timestamp.

## Verification

- Verify bidirectional forwarding on port `8080`.
- Verify that port `8081` still produces the raw byte stream.
- Run `python/debug_read.py --host <gateway> --output captures/test` and check sequence continuity plus RX/TX timestamps.
- Compare `TX_COMPLETE` with the next `RX_READ` to detect overlap or insufficient bus idle time.
