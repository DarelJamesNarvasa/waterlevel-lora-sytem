// Station 1 with integrated sd card code.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <esp_system.h>

// =====================================================
// STATION 1: ESP32 + SX1278 + MB7389 + MICROSD
// =====================================================

// ================= LoRa Pin Configuration ============

#define LORA_SCK 18
#define LORA_MISO 23
#define LORA_MOSI 19
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

// ================= MicroSD Pin Configuration =========

#define SD_SCK 25
#define SD_MISO 32
#define SD_MOSI 33
#define SD_CS 26

// ================= Sensor Pin Configuration ==========

#define MB7389_PULSE_PIN 27

// ================= LoRa Configuration ================

const long LORA_FREQUENCY = 433E6;
const byte LORA_SYNC_WORD = 0xF3;

// ================= Timing Configuration ==============

const unsigned long PULSE_TIMEOUT_MICROSECONDS = 250000;

const unsigned long SEND_INTERVAL_MILLISECONDS = 5000;

// ================= MicroSD Configuration =============

const char* SD_FILE_NAME = "/station1.csv";

const uint32_t SD_SPI_FREQUENCY = 4000000;

// Separate HSPI bus for MicroSD
SPIClass sdSPI(HSPI);

// ================= Global Variables ==================

uint32_t sessionId = 0;
uint32_t packetId = 0;

unsigned long previousSendTime = 0;

bool sdReady = false;

// =====================================================
// INITIALIZE LORA
// =====================================================

void initializeLoRa() {

  Serial.println();
  Serial.println("Initializing LoRa...");

  // Existing LoRa SPI connection
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

// =====================================================
// INITIALIZE MICROSD CARD
// =====================================================

void initializeSD() {

  Serial.println();
  Serial.println("Initializing MicroSD Card...");

  sdReady = false;

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // Initialize separate SPI bus
  sdSPI.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  if (!SD.begin(SD_CS, sdSPI, SD_SPI_FREQUENCY)) {

    Serial.println("MicroSD initialization failed!");
    Serial.println("LoRa will continue without SD logging.");

    return;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {

    Serial.println("No MicroSD card detected!");
    return;
  }

  Serial.println("MicroSD initialized successfully!");

  Serial.print("Card Type: ");

  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  }
  else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  }
  else if (cardType == CARD_SDHC) {
    Serial.println("SDHC / SDXC");
  }
  else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSizeMB =
    SD.cardSize() / (1024ULL * 1024ULL);

  Serial.print("Card Size: ");
  Serial.print(cardSizeMB);
  Serial.println(" MB");

  sdReady = true;

  // Create CSV file if missing
  if (!SD.exists(SD_FILE_NAME)) {

    File file = SD.open(
      SD_FILE_NAME,
      FILE_WRITE
    );

    if (!file) {

      Serial.println("Failed to create CSV file!");
      sdReady = false;

      return;
    }

    file.println(
      "Uptime_ms,"
      "Session_ID,"
      "Packet_ID,"
      "Sensor_Status,"
      "Pulse_Duration_us,"
      "Distance_mm,"
      "LoRa_TX_Complete"
    );

    file.flush();

    bool headerOK =
      file.getWriteError() == 0;

    file.close();

    if (!headerOK) {

      Serial.println("Failed to write CSV header!");
      sdReady = false;

      return;
    }

    Serial.println("CSV file created successfully!");
  }
  else {

    Serial.println("Existing CSV file found.");
    Serial.println("New readings will be appended.");
  }

  Serial.println("MicroSD ready for data logging.");
}

// =====================================================
// SAVE SENSOR READING TO MICROSD
// =====================================================

bool saveToSD(
  unsigned long timestamp,
  uint32_t currentSessionId,
  uint32_t currentPacketId,
  unsigned long pulseDuration,
  bool transmissionSuccess
) {

  if (!sdReady) {

    Serial.println("SD logging skipped: card unavailable.");

    return false;
  }

  File file = SD.open(
    SD_FILE_NAME,
    FILE_APPEND
  );

  if (!file) {

    Serial.println("Failed to open CSV file!");

    return false;
  }

  // Column 1: ESP32 uptime in milliseconds
  file.print(timestamp);
  file.print(",");

  // Column 2: Session ID
  file.print(currentSessionId);
  file.print(",");

  // Column 3: Packet ID
  file.print(currentPacketId);
  file.print(",");

  // Column 4: Sensor Status
  if (pulseDuration == 0) {
    file.print("no_pulse");
  }
  else {
    file.print("ok");
  }

  file.print(",");

  // Column 5: Pulse Duration
  file.print(pulseDuration);
  file.print(",");

  // Column 6: Distance in Millimeters
  if (pulseDuration > 0) {

    file.print(pulseDuration);
  }

  file.print(",");

  // Column 7: Local LoRa transmission completion
  file.println(transmissionSuccess ? 1 : 0);

  // Flush buffered data
  file.flush();

  bool writeSuccess =
    file.getWriteError() == 0;

  file.close();

  if (!writeSuccess) {

    Serial.println("MicroSD write error!");

    return false;
  }

  Serial.println("Data successfully saved to MicroSD!");

  return true;
}

// =====================================================
// TRANSMIT JSON THROUGH LORA
// =====================================================

bool transmitJson(const String &jsonData) {

  if (jsonData.length() > 255) {

    Serial.print("JSON is too large: ");
    Serial.print(jsonData.length());
    Serial.println(" bytes");

    return false;
  }

  LoRa.idle();

  if (LoRa.beginPacket() != 1) {

    Serial.println("Failed to begin LoRa packet");

    return false;
  }

  LoRa.print(jsonData);

  int transmissionResult = LoRa.endPacket();

  return transmissionResult == 1;
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  pinMode(MB7389_PULSE_PIN, INPUT);

  delay(1000);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" STATION 1 WATER LEVEL MONITORING");
  Serial.println(" ESP32 + LoRa + MB7389 + MicroSD");
  Serial.println("====================================");

  // Generate random session ID
  sessionId = esp_random();

  // Initialize LoRa
  initializeLoRa();

  // Initialize MicroSD
  initializeSD();

  // Preserve immediate first transmission
  previousSendTime =
    millis() - SEND_INTERVAL_MILLISECONDS;

  Serial.println();
  Serial.println("Station 1 ready");

  Serial.print("Session ID: ");
  Serial.println(sessionId);

  Serial.print("MicroSD Status: ");

  if (sdReady) {
    Serial.println("READY");
  }
  else {
    Serial.println("UNAVAILABLE");
  }
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // Maintain 5-second interval
  if (
    millis() - previousSendTime <
    SEND_INTERVAL_MILLISECONDS
  ) {
    return;
  }

  previousSendTime = millis();

  unsigned long timestamp = millis();

  // ===================================================
  // STEP 1: READ MB7389 ULTRASONIC SENSOR
  // ===================================================

  unsigned long pulseDuration = pulseIn(
    MB7389_PULSE_PIN,
    HIGH,
    PULSE_TIMEOUT_MICROSECONDS
  );

  // ===================================================
  // STEP 2: PREPARE JSON DATA
  // ===================================================

  JsonDocument document;

  document["sourceStation"] = "station1";
  document["destinationStation"] = "station2";
  document["sessionId"] = sessionId;
  document["packetId"] = packetId;

  if (pulseDuration == 0) {

    document["sensorStatus"] = "no_pulse";

    document["pulseDurationMicroseconds"] = 0;

    document["distanceMillimeters"] = nullptr;
  }
  else {

    document["sensorStatus"] = "ok";

    document["pulseDurationMicroseconds"] =
      pulseDuration;

    document["distanceMillimeters"] =
      pulseDuration;
  }

  String jsonData;

  serializeJson(document, jsonData);

  // ===================================================
  // STEP 3: DISPLAY SENSOR READING
  // ===================================================

  Serial.println();
  Serial.println("--------------------------------");

  Serial.print("Session ID: ");
  Serial.println(sessionId);

  Serial.print("Packet ID: ");
  Serial.println(packetId);

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
  else {

    Serial.println("Sensor Status: NO PULSE");
  }

  // ===================================================
  // STEP 4: TRANSMIT JSON THROUGH LORA
  // ===================================================

  bool transmissionSuccess = transmitJson(jsonData);

  if (transmissionSuccess) {

    Serial.println("Packet transmitted to Station 2");
  }
  else {

    Serial.println("Packet transmission failed");
  }

  // ===================================================
  // STEP 5: SAVE SENSOR DATA TO MICROSD
  // ===================================================

  bool saveSuccess = saveToSD(
    timestamp,
    sessionId,
    packetId,
    pulseDuration,
    transmissionSuccess
  );

  if (saveSuccess) {

    Serial.println("SD Logging: SUCCESS");
  }
  else {

    Serial.println("SD Logging: FAILED / UNAVAILABLE");
  }

  // ===================================================
  // STEP 6: INCREMENT PACKET ID
  // ===================================================

  packetId++;

  Serial.println("--------------------------------");
}
