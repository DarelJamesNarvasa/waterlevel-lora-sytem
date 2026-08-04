#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>

#define LORA_SCK 18
#define LORA_MISO 23
#define LORA_MOSI 19
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

const long LORA_FREQUENCY = 433E6;
const byte LORA_SYNC_WORD = 0xF3;

bool hasPreviousPacket = false;

uint32_t previousSessionId = 0;
uint32_t previousPacketId = 0;

void initializeLoRa() {
  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS
  );

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0
  );

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa initialization failed");

    while (true) {
      delay(1000);
    }
  }

  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setSpreadingFactor(7);
  LoRa.setCodingRate4(5);
  LoRa.setTxPower(17);
  LoRa.enableCrc();

  LoRa.receive();

  Serial.println("LoRa relay initialized");
}

String readLoRaPacket(int packetSize) {
  String receivedData;
  receivedData.reserve(packetSize);

  while (LoRa.available()) {
    receivedData +=
      static_cast<char>(LoRa.read());
  }

  return receivedData;
}

bool transmitJson(const String &jsonData) {
  if (jsonData.length() > 255) {
    Serial.print("Relay JSON is too large: ");
    Serial.print(jsonData.length());
    Serial.println(" bytes");

    return false;
  }

  LoRa.idle();

  delay(50);

  LoRa.beginPacket();
  LoRa.print(jsonData);

  int transmissionResult = LoRa.endPacket();

  LoRa.receive();

  return transmissionResult == 1;
}

void setup() {
  Serial.begin(115200);

  delay(1000);

  initializeLoRa();

  Serial.println("Station 2 ready");
  Serial.println("Waiting for Station 1");
}

void loop() {
  int packetSize = LoRa.parsePacket();

  if (packetSize <= 0) {
    return;
  }

  String receivedJson =
    readLoRaPacket(packetSize);

  int receivedRssiDbm =
    LoRa.packetRssi();

  float receivedSnrDb =
    LoRa.packetSnr();

  Serial.println("--------------------------------");

  Serial.print("Received JSON: ");
  Serial.println(receivedJson);

  Serial.print("Received RSSI: ");
  Serial.print(receivedRssiDbm);
  Serial.println(" dBm");

  Serial.print("Received SNR: ");
  Serial.print(receivedSnrDb, 2);
  Serial.println(" dB");

  JsonDocument document;

  DeserializationError jsonError =
    deserializeJson(document, receivedJson);

  if (jsonError) {
    Serial.print("Invalid JSON: ");
    Serial.println(jsonError.c_str());

    return;
  }

  const char *sourceStation =
    document["sourceStation"] | "";

  const char *destinationStation =
    document["destinationStation"] | "";

  if (
    strcmp(sourceStation, "station1") != 0 ||
    strcmp(destinationStation, "station2") != 0
  ) {
    Serial.println(
      "Packet is not intended for Station 2"
    );

    return;
  }

  if (
    document["sessionId"].isNull() ||
    document["packetId"].isNull()
  ) {
    Serial.println(
      "Session ID or packet ID is missing"
    );

    return;
  }

  uint32_t receivedSessionId =
    document["sessionId"];

  uint32_t receivedPacketId =
    document["packetId"];

  if (
    hasPreviousPacket &&
    receivedSessionId == previousSessionId &&
    receivedPacketId == previousPacketId
  ) {
    Serial.println("Duplicate packet ignored");

    return;
  }

  document["destinationStation"] =
    "station3";

  document["relayReceivedRssiDbm"] =
    receivedRssiDbm;

  document["relayReceivedSnrDb"] =
    receivedSnrDb;

  String relayJson;
  serializeJson(document, relayJson);

  Serial.print("Relaying JSON: ");
  Serial.println(relayJson);

  Serial.print("Payload size: ");
  Serial.print(relayJson.length());
  Serial.println(" bytes");

  if (transmitJson(relayJson)) {
    previousSessionId =
      receivedSessionId;

    previousPacketId =
      receivedPacketId;

    hasPreviousPacket = true;

    Serial.println(
      "Packet transmitted to Station 3"
    );
  } else {
    Serial.println(
      "Transmission to Station 3 failed"
    );
  }
}