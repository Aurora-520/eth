#define ETH_CLK_MODE        ETH_CLOCK_GPIO0_OUT
#define ETH_PHY_TYPE        ETH_PHY_LAN8720
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC         23
#define ETH_PHY_MDIO        18
#define ETH_PHY_POWER       12 //接12

#include <Arduino.h>
#include <ETH.h>
#include <HardwareSerial.h>
#include <WiFiUdp.h>

// 电脑有线网卡也要在 192.168.102.x/24，。
IPAddress localIP(192, 168, 102, 166);
IPAddress gateway(192, 168, 102, 1);
IPAddress subnet(255, 255, 255, 0);
#define UDP_LISTEN_PORT     8080

//要共地。
#define STM32_RX_PIN        16
#define STM32_TX_PIN        17
#define STM32_BAUD_RATE     100000


#define UDP_BUFFER_SIZE     512
#define UART_BUFFER_SIZE    512
#define UART_COLLECT_MS     2
#define ETH_STATUS_LOG_MS   5000

WiFiUDP UdpServer;
HardwareSerial STM32Serial(1);

bool ethReady = false;
bool hasClient = false;
bool ethLinkUp = false;
IPAddress lastClientIP;
uint16_t lastClientPort = 0;
uint32_t lastEthStatusLogMs = 0;

// 串口日志
static void printHex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    Serial.printf("%02X ", data[i]);
  }
  Serial.println();
}

// 带上 link/IP/MAC，
static void printEthStatus(const char *tag) {
  Serial.printf("[ETH] %s link=%s speed=%uMbps fullDuplex=%s ready=%s ip=%s mac=%s\n",
                tag,
                ETH.linkUp() ? "up" : "down",
                ETH.linkSpeed(),
                ETH.fullDuplex() ? "yes" : "no",
                ethReady ? "yes" : "no",
                ETH.localIP().toString().c_str(),
                ETH.macAddress().c_str());
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[ETH] Driver started");
      ETH.setHostname("esp32-udp-uart");
      Serial.printf("[ETH] MAC: %s\n", ETH.macAddress().c_str());
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[ETH] Link up");
      ethLinkUp = true;
      printEthStatus("connected");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      // 这里到达后，UDP 才开始监听。
      Serial.print("[ETH] IP: ");
      Serial.println(ETH.localIP());
      printEthStatus("got-ip");

      if (UdpServer.begin(UDP_LISTEN_PORT)) {
        Serial.printf("[UDP] Listening on port %d\n", UDP_LISTEN_PORT);
        ethReady = true;
      } else {
        Serial.println("[UDP] Failed to bind port");
      }
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("[ETH] Link down");
      ethLinkUp = false;
      ethReady = false;
      hasClient = false;
      UdpServer.stop();
      printEthStatus("disconnected");
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

 
  STM32Serial.setRxBufferSize(1024);
  STM32Serial.begin(STM32_BAUD_RATE, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
  Serial.printf("[UART] STM32 serial started: RX=%d TX=%d baud=%d\n",
                STM32_RX_PIN, STM32_TX_PIN, STM32_BAUD_RATE);

  WiFi.onEvent(WiFiEvent);

  Serial.println("[ETH] Initializing Ethernet...");
  if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC,
                 ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
    Serial.println("[ETH] Init failed, stopped");
    while (true) {
      delay(1000);
    }
  }

  delay(100);
  // 静态 IP
  if (ETH.config(localIP, gateway, subnet)) {
    Serial.printf("[ETH] Static IP requested: %s\n", localIP.toString().c_str());
  } else {
    Serial.println("[ETH] Static IP config failed");
  }
  printEthStatus("after-config");
}

static void handleUdpToUart() {
  // 电脑来的 UDP 包，只要收到就马上转给 STM32。
  int packetSize = UdpServer.parsePacket();
  if (packetSize <= 0) {
    return;
  }

  lastClientIP = UdpServer.remoteIP();
  lastClientPort = UdpServer.remotePort();
  hasClient = true;

  uint8_t buffer[UDP_BUFFER_SIZE];
  int remaining = packetSize;
  int totalRead = 0;
  size_t totalWritten = 0;

  while (remaining > 0) {
    int toRead = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
    int len = UdpServer.read(buffer, toRead);
    if (len <= 0) {
      break;
    }

    // 这里写成功只能说明 ESP32 已经往 TX 发了，不代表 STM32 一定收到
    size_t written = STM32Serial.write(buffer, len);
    totalRead += len;
    totalWritten += written;
    remaining -= len;

    Serial.printf("[UDP->UART] %s:%u, %d bytes\n",
                  lastClientIP.toString().c_str(), lastClientPort, len);
    Serial.print("[UDP->UART] HEX: ");
    printHex(buffer, len);

    if (written != (size_t)len) {
      Serial.printf("[UART] Warning: wanted %d bytes, wrote %u bytes\n",
                    len, (unsigned)written);
    }
  }

  Serial.printf("[UDP->UART] Packet done: UDP %d bytes, UART %u bytes\n",
                totalRead, (unsigned)totalWritten);
}

static void handleUartToUdp() {
  // STM32 有回复才会进这里
  if (!STM32Serial.available()) {
    return;
  }

  // 稍微等一下
  delay(UART_COLLECT_MS);

  uint8_t buffer[UART_BUFFER_SIZE];
  size_t len = 0;

  while (STM32Serial.available() && len < sizeof(buffer)) {
    int b = STM32Serial.read();
    if (b < 0) {
      break;
    }
    buffer[len++] = (uint8_t)b;
  }

  if (len == 0) {
    return;
  }

  Serial.printf("[UART->UDP] %u bytes\n", (unsigned)len);
  Serial.print("[UART->UDP] HEX: ");
  printHex(buffer, len);

  if (!ethReady || !hasClient) {
    // 先不发 UDP
    Serial.println("[UART->UDP] No UDP client yet, dropped");
    return;
  }

  UdpServer.beginPacket(lastClientIP, lastClientPort);
  UdpServer.write(buffer, len);
  UdpServer.endPacket();
  Serial.printf("[UART->UDP] Sent to %s:%u\n",
                lastClientIP.toString().c_str(), lastClientPort);
}

void loop() {
  // 5 秒打一条状态日志，方便排查问题。
  if (millis() - lastEthStatusLogMs >= ETH_STATUS_LOG_MS) {
    lastEthStatusLogMs = millis();
    printEthStatus("heartbeat");
  }

  if (!ethReady) {
    delay(100);
    return;
  }

  handleUdpToUart();
  handleUartToUdp();
  delay(1);
}
