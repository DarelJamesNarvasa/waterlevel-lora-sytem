#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <esp_system.h>

#define LORA_SCK 18
#define LORA_MISO 23
#define LORA_MOSI 19
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

#define MB7389_PULSE_PIN 27

const long LORA_FREQUENCY = 433E6;
const byte LORA_SYNC_WORD = 0xF3;

const unsigned long PULSE_TIMEOUT_MICROSECONDS = 250000;
const unsigned long SEND_INTERVAL_MILLISECONDS = 5000;

uint32_t sessionId = 0;
uint32_t packetId = 0;

unsigned long previousSendTime = 0;

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

  Serial.println("LoRa initialized successfully");
}

bool transmitJson(const String &jsonData) {
  if (jsonData.length() > 255) {
    Serial.print("JSON is too large: ");
    Serial.print(jsonData.length());
    Serial.println(" bytes");

    return false;
  }

  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(jsonData);

  int transmissionResult = LoRa.endPacket();

  return transmissionResult == 1;
}

void setup() {
  Serial.begin(115200);

  pinMode(MB7389_PULSE_PIN, INPUT);

  delay(1000);

  sessionId = esp_random();

  initializeLoRa();

  previousSendTime =
    millis() - SEND_INTERVAL_MILLISECONDS;

  Serial.println("Station 1 ready");

  Serial.print("Session ID: ");
  Serial.println(sessionId);
}

void loop() {
  if (
    millis() - previousSendTime <
    SEND_INTERVAL_MILLISECONDS
  ) {
    return;
  }

  previousSendTime = millis();

  unsigned long pulseDuration = pulseIn(
    MB7389_PULSE_PIN,
    HIGH,
    PULSE_TIMEOUT_MICROSECONDS
  );

  JsonDocument document;

  document["sourceStation"] = "station1";
  document["destinationStation"] = "station2";
  document["sessionId"] = sessionId;
  document["packetId"] = packetId;

  if (pulseDuration == 0) {
    document["sensorStatus"] = "no_pulse";
    document["pulseDurationMicroseconds"] = 0;
    document["distanceMillimeters"] = nullptr;
  } else {
    document["sensorStatus"] = "ok";
    document["pulseDurationMicroseconds"] =
      pulseDuration;

    document["distanceMillimeters"] =
      pulseDuration;
  }

  String jsonData;
  serializeJson(document, jsonData);

  Serial.println("--------------------------------");

  Serial.print("Sending JSON: ");
  Serial.println(jsonData);

  Serial.print("Payload size: ");
  Serial.print(jsonData.length());
  Serial.println(" bytes");

  if (pulseDuration > 0) {
    float distanceCentimeters =
      pulseDuration / 10.0f;

    float distanceMeters =
      pulseDuration / 1000.0f;

    Serial.print("Distance: ");
    Serial.print(pulseDuration);
    Serial.print(" mm | ");

    Serial.print(distanceCentimeters, 1);
    Serial.print(" cm | ");

    Serial.print(distanceMeters, 2);
    Serial.println(" m");
  }

  if (transmitJson(jsonData)) {
    Serial.println("Packet transmitted to Station 2");
  } else {
    Serial.println("Packet transmission failed");
  }

  packetId++;
}
