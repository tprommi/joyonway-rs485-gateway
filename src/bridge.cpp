#include "bridge.h"
#include "led.h"
#include <WiFi.h>
#include <esp_timer.h>

namespace {
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
WiFiServer tcpSnifferServer(TCP_SNIFFER_PORT);
WiFiClient tcpSnifferClient;
WiFiServer tcpDebugServer(TCP_DEBUG_PORT);
WiFiClient tcpDebugClient;
uint32_t debugSequence = 0;
bool controlClientWasConnected = false;

const int RS485_TX_PIN = 17;
const int RS485_RX_PIN = 18;
const int RS485_BAUD = 38400;
const bool RS485_UART_INVERT = false;
HardwareSerial rs485Serial(2);

const uint8_t SYNC_FRAME[] = {
  0x1A, 0x01, 0x20, 0x08, 0x3C, 0xAA, 0x10,
  0x00, 0x00, 0x6B, 0x73, 0xE4, 0xB9, 0x1D,
};
const size_t MAX_COMMAND_FRAME_SIZE = 128;
const size_t TX_QUEUE_CAPACITY = 8;
const uint64_t TX_POLL_GUARD_US = 10000;
const uint64_t TX_QUEUE_TIMEOUT_US = 5000000;

struct QueuedCommand {
  uint8_t data[MAX_COMMAND_FRAME_SIZE];
  size_t len;
  uint64_t queuedAtUs;
};

QueuedCommand txQueue[TX_QUEUE_CAPACITY];
size_t txQueueHead = 0;
size_t txQueueCount = 0;
uint64_t scheduledTxUs = 0;
bool waitingForPoll21 = false;
uint8_t tcpCommandFrame[MAX_COMMAND_FRAME_SIZE];
size_t tcpCommandFrameLen = 0;
bool tcpCommandInFrame = false;
uint8_t rs485Frame[MAX_COMMAND_FRAME_SIZE];
size_t rs485FrameLen = 0;
bool rs485InFrame = false;

void writeSniffer(const uint8_t* data, size_t len) {
  if (len == 0) return;

  if (tcpSnifferClient && tcpSnifferClient.connected()) {
    tcpSnifferClient.write(data, len);
  }
}

enum class DebugEvent : uint8_t {
  RxRead = 1,
  TxStart = 2,
  TxQueued = 3,
  TxComplete = 4,
  CommandAccepted = 5,
  SyncArmed = 6,
  CommandDropped = 7,
  Poll21Armed = 8,
};

void writeDebugUint16(uint16_t value) {
  uint8_t bytes[] = {
    static_cast<uint8_t>(value >> 8),
    static_cast<uint8_t>(value),
  };
  tcpDebugClient.write(bytes, sizeof(bytes));
}

void writeDebugUint32(uint32_t value) {
  uint8_t bytes[] = {
    static_cast<uint8_t>(value >> 24),
    static_cast<uint8_t>(value >> 16),
    static_cast<uint8_t>(value >> 8),
    static_cast<uint8_t>(value),
  };
  tcpDebugClient.write(bytes, sizeof(bytes));
}

void writeDebugUint64(uint64_t value) {
  uint8_t bytes[8];
  for (int i = 7; i >= 0; i--) {
    bytes[7 - i] = static_cast<uint8_t>(value >> (i * 8));
  }
  tcpDebugClient.write(bytes, sizeof(bytes));
}

void writeDebugRecord(DebugEvent event, uint64_t timestampUs, const uint8_t* data, size_t len) {
  if (!tcpDebugClient || !tcpDebugClient.connected() || len > UINT16_MAX) return;

  static const uint8_t magic[] = {'J', 'W', 'D', 'B'};
  tcpDebugClient.write(magic, sizeof(magic));
  tcpDebugClient.write(1);
  tcpDebugClient.write(static_cast<uint8_t>(event));
  writeDebugUint32(debugSequence++);
  writeDebugUint64(timestampUs);
  writeDebugUint16(static_cast<uint16_t>(len));
  if (len > 0) tcpDebugClient.write(data, len);
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void clearTxQueue(const char* reason) {
  while (txQueueCount > 0) {
    QueuedCommand& command = txQueue[txQueueHead];
    writeDebugRecord(DebugEvent::CommandDropped, esp_timer_get_time(), command.data, command.len);
    txQueueHead = (txQueueHead + 1) % TX_QUEUE_CAPACITY;
    txQueueCount--;
  }
  scheduledTxUs = 0;
  waitingForPoll21 = false;
  tcpCommandFrameLen = 0;
  tcpCommandInFrame = false;
  Serial.printf("RS485 TX-Warteschlange geleert: %s\n", reason);
}

void enqueueCommand(const uint8_t* data, size_t len, uint64_t timestampUs) {
  if (len == 0 || len > MAX_COMMAND_FRAME_SIZE) return;
  if (txQueueCount >= TX_QUEUE_CAPACITY) {
    writeDebugRecord(DebugEvent::CommandDropped, timestampUs, data, len);
    Serial.println("RS485 TX-Warteschlange voll, Befehl verworfen");
    return;
  }

  size_t tail = (txQueueHead + txQueueCount) % TX_QUEUE_CAPACITY;
  memcpy(txQueue[tail].data, data, len);
  txQueue[tail].len = len;
  txQueue[tail].queuedAtUs = timestampUs;
  txQueueCount++;
  writeDebugRecord(DebugEvent::CommandAccepted, timestampUs, data, len);
}

void processTcpCommandByte(uint8_t byteValue) {
  if (!tcpCommandInFrame) {
    if (byteValue != 0x1A) return;
    tcpCommandInFrame = true;
    tcpCommandFrameLen = 0;
  } else if (byteValue == 0x1A) {
    tcpCommandFrameLen = 0;
  }

  if (tcpCommandFrameLen >= sizeof(tcpCommandFrame)) {
    writeDebugRecord(
      DebugEvent::CommandDropped,
      esp_timer_get_time(),
      tcpCommandFrame,
      tcpCommandFrameLen
    );
    tcpCommandFrameLen = 0;
    tcpCommandInFrame = false;
    return;
  }

  tcpCommandFrame[tcpCommandFrameLen++] = byteValue;
  if (byteValue == 0x1D) {
    enqueueCommand(tcpCommandFrame, tcpCommandFrameLen, esp_timer_get_time());
    tcpCommandFrameLen = 0;
    tcpCommandInFrame = false;
  }
}

void processRs485FrameByte(uint8_t byteValue, uint64_t timestampUs) {
  if (!rs485InFrame) {
    if (byteValue != 0x1A) return;
    rs485InFrame = true;
    rs485FrameLen = 0;
  } else if (byteValue == 0x1A) {
    rs485FrameLen = 0;
  }

  if (rs485FrameLen >= sizeof(rs485Frame)) {
    rs485FrameLen = 0;
    rs485InFrame = false;
    return;
  }

  rs485Frame[rs485FrameLen++] = byteValue;
  if (byteValue != 0x1D) return;

  bool isSync = rs485FrameLen == sizeof(SYNC_FRAME)
    && memcmp(rs485Frame, SYNC_FRAME, sizeof(SYNC_FRAME)) == 0;
  bool isPoll21 = rs485FrameLen > 1 && rs485Frame[1] == 0x21;
  rs485FrameLen = 0;
  rs485InFrame = false;
  if (isSync && txQueueCount > 0 && scheduledTxUs == 0) {
    waitingForPoll21 = true;
    writeDebugRecord(DebugEvent::SyncArmed, timestampUs, nullptr, 0);
  } else if (isPoll21 && waitingForPoll21 && txQueueCount > 0) {
    scheduledTxUs = timestampUs + TX_POLL_GUARD_US;
    waitingForPoll21 = false;
    writeDebugRecord(DebugEvent::Poll21Armed, timestampUs, nullptr, 0);
  }
}

void dropExpiredCommands(uint64_t nowUs) {
  while (txQueueCount > 0) {
    QueuedCommand& command = txQueue[txQueueHead];
    if (nowUs - command.queuedAtUs <= TX_QUEUE_TIMEOUT_US) return;
    writeDebugRecord(DebugEvent::CommandDropped, nowUs, command.data, command.len);
    txQueueHead = (txQueueHead + 1) % TX_QUEUE_CAPACITY;
    txQueueCount--;
    scheduledTxUs = 0;
    waitingForPoll21 = false;
    Serial.println("RS485 TX-Befehl wegen Timeout verworfen");
  }
}

void handleScheduledTx() {
  uint64_t nowUs = esp_timer_get_time();
  dropExpiredCommands(nowUs);
  if (txQueueCount == 0 || scheduledTxUs == 0 || nowUs < scheduledTxUs) return;

  QueuedCommand& command = txQueue[txQueueHead];
  uint64_t txStartUs = esp_timer_get_time();
  size_t queuedLen = rs485Serial.write(command.data, command.len);
  uint64_t txQueuedUs = esp_timer_get_time();
  writeSniffer(command.data, command.len);
  rs485Serial.flush();
  uint64_t txCompleteUs = esp_timer_get_time();

  writeDebugRecord(DebugEvent::TxStart, txStartUs, command.data, command.len);
  writeDebugRecord(DebugEvent::TxQueued, txQueuedUs, nullptr, 0);
  writeDebugRecord(DebugEvent::TxComplete, txCompleteUs, nullptr, 0);

  if (queuedLen != command.len) {
    Serial.printf(
      "RS485 TX unvollstaendig: %u von %u Bytes eingereiht\n",
      (unsigned)queuedLen,
      (unsigned)command.len
    );
  }
  txQueueHead = (txQueueHead + 1) % TX_QUEUE_CAPACITY;
  txQueueCount--;
  scheduledTxUs = 0;
  waitingForPoll21 = false;
}

void handleRs485() {
  while (rs485Serial.available()) {
    uint8_t chunk[64];
    size_t chunkLen = rs485Serial.readBytes(chunk, min((size_t)rs485Serial.available(), sizeof(chunk)));
    if (chunkLen == 0) {
      return;
    }
    uint64_t readTimestampUs = esp_timer_get_time();

    // direkt unverändert weiterleiten: TCP bleibt ein reiner Binärstream
    if (tcpClient && tcpClient.connected()) {
      tcpClient.write(chunk, chunkLen);
    }
    writeSniffer(chunk, chunkLen);
    writeDebugRecord(DebugEvent::RxRead, readTimestampUs, chunk, chunkLen);

    for (size_t i = 0; i < chunkLen; i++) {
      processRs485FrameByte(chunk[i], readTimestampUs);
    }

  }
}

bool handleTcpClient(WiFiServer& server, WiFiClient& client, const char* label) {
  if (client && !client.connected()) {
    client.stop();
  }

  if (!server.hasClient()) {
    return false;
  }

  WiFiClient newClient = server.accept();
  if (!newClient) {
    return false;
  }

  if (client.connected()) {
    client.stop();
  }

  client = newClient;
  client.setNoDelay(true);
  Serial.printf("%s verbunden: %s\n", label, client.remoteIP().toString().c_str());
  setRgbColor(64, 64, 64);
  return true;
}

void discardSnifferInput() {
  if (!tcpSnifferClient || !tcpSnifferClient.connected()) {
    return;
  }

  while (tcpSnifferClient.available()) {
    uint8_t trash[64];
    tcpSnifferClient.readBytes(trash, min((size_t)tcpSnifferClient.available(), sizeof(trash)));
  }
}

void discardDebugInput() {
  if (!tcpDebugClient || !tcpDebugClient.connected()) {
    return;
  }

  while (tcpDebugClient.available()) {
    uint8_t trash[64];
    tcpDebugClient.readBytes(trash, min((size_t)tcpDebugClient.available(), sizeof(trash)));
  }
}

} // namespace

void initRs485() {
  rs485Serial.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, RS485_UART_INVERT);
}

void startBridgeServers() {
  tcpServer.begin();
  tcpSnifferServer.begin();
  tcpDebugServer.begin();
  Serial.printf("TCP-Server gestartet auf Port %d\n", TCP_PORT);
  Serial.printf("TCP-Sniffer gestartet auf Port %d\n", TCP_SNIFFER_PORT);
  Serial.printf("TCP-Debug gestartet auf Port %d\n", TCP_DEBUG_PORT);
}

void stopBridgeClients() {
  clearTxQueue("Netzwerk getrennt");
  tcpClient.stop();
  tcpSnifferClient.stop();
  tcpDebugClient.stop();
}

void pollBridge() {
  bool controlClientReplaced = handleTcpClient(tcpServer, tcpClient, "TCP-Client");
  bool controlClientConnected = tcpClient && tcpClient.connected();
  if (controlClientReplaced) {
    clearTxQueue("TCP-Steuerclient verbunden");
  } else if (controlClientWasConnected && !controlClientConnected) {
    clearTxQueue("TCP-Steuerclient getrennt");
  }
  controlClientWasConnected = controlClientConnected;
  handleTcpClient(tcpSnifferServer, tcpSnifferClient, "TCP-Sniffer");
  handleTcpClient(tcpDebugServer, tcpDebugClient, "TCP-Debug");
  discardSnifferInput();
  discardDebugInput();

  handleScheduledTx();

  if (tcpClient && tcpClient.connected()) {
    while (tcpClient.available()) {
      uint8_t chunk[64];
      size_t chunkLen = tcpClient.readBytes(chunk, min((size_t)tcpClient.available(), sizeof(chunk)));
      if (chunkLen == 0) break;
      for (size_t i = 0; i < chunkLen; i++) {
        processTcpCommandByte(chunk[i]);
      }
    }
  }

  handleRs485();
  handleScheduledTx();
}
