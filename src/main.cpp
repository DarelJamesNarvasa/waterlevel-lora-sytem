#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#define LORA_SCK 18
#define LORA_MISO 23
#define LORA_MOSI 19
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

#define WIFI_SSID "DurangoV3"
#define WIFI_PASSWORD "ToJapan2025!"

const char *FIREBASE_DATABASE_URL =
  "https://testing-fc60d-default-rtdb.asia-southeast1.firebasedatabase.app";

const long LORA_FREQUENCY = 433E6;
const byte LORA_SYNC_WORD = 0xF3;

const float SENSOR_TO_ZERO_WATER_LEVEL_MILLIMETERS = 0.0f;

bool hasPreviousPacket = false;
uint32_t previousSessionId = 0;
uint32_t previousPacketId = 0;

bool hasPendingUpload = false;
String pendingFirebaseJson = "";

uint32_t pendingSessionId = 0;
uint32_t pendingPacketId = 0;

unsigned long previousWiFiReconnectTime = 0;

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
  LoRa.enableCrc();
  LoRa.receive();

  Serial.println("LoRa initialized successfully");
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  unsigned long connectionStartTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - connectionStartTime < 20000
  ) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Wi-Fi RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("Wi-Fi connection failed");
    Serial.println("Station 3 will continue trying");
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - previousWiFiReconnectTime < 10000) {
    return;
  }

  previousWiFiReconnectTime = millis();

  Serial.println("Attempting Wi-Fi reconnection");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

String readLoRaPacket(int packetSize) {
  String receivedJson;
  receivedJson.reserve(packetSize + 1);

  while (LoRa.available()) {
    receivedJson += static_cast<char>(LoRa.read());
  }

  return receivedJson;
}

String getUtcTimestamp() {
  struct tm timeInformation;

  if (!getLocalTime(&timeInformation, 100)) {
    return "";
  }

  char timestampBuffer[30];

  strftime(
    timestampBuffer,
    sizeof(timestampBuffer),
    "%Y-%m-%dT%H:%M:%SZ",
    &timeInformation
  );

  return String(timestampBuffer);
}

bool sendJsonToFirebase(
  const String &databasePath,
  const String &jsonData
) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase upload skipped: Wi-Fi disconnected");
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient httpClient;

  String firebaseUrl = String(FIREBASE_DATABASE_URL);
  firebaseUrl += databasePath;
  firebaseUrl += ".json";

  Serial.print("Firebase URL: ");
  Serial.println(firebaseUrl);

  if (!httpClient.begin(secureClient, firebaseUrl)) {
    Serial.println("Unable to start HTTPS connection");
    return false;
  }

  httpClient.addHeader(
    "Content-Type",
    "application/json"
  );

  httpClient.setTimeout(15000);

  int httpResponseCode =
    httpClient.PUT(jsonData);

  String firebaseResponse =
    httpClient.getString();

  Serial.print("Firebase HTTP response: ");
  Serial.println(httpResponseCode);

  if (firebaseResponse.length() > 0) {
    Serial.print("Firebase response data: ");
    Serial.println(firebaseResponse);
  }

  httpClient.end();

  if (
    httpResponseCode >= 200 &&
    httpResponseCode < 300
  ) {
    return true;
  }

  return false;
}

void prepareFirebaseJson(
  JsonDocument &receivedDocument,
  int station2ToStation3RssiDbm,
  float station2ToStation3SnrDb
) {
  JsonDocument firebaseDocument;

  uint32_t sessionId =
    receivedDocument["sessionId"];

  uint32_t packetId =
    receivedDocument["packetId"];

  const char *sensorStatus =
    receivedDocument["sensorStatus"] | "unknown";

  firebaseDocument["deviceId"] =
    "WATER-LEVEL-001";

  firebaseDocument["sourceStation"] =
    "station1";

  firebaseDocument["relayStation"] =
    "station2";

  firebaseDocument["gatewayStation"] =
    "station3";

  firebaseDocument["sessionId"] =
    sessionId;

  firebaseDocument["packetId"] =
    packetId;

  firebaseDocument["sensorStatus"] =
    sensorStatus;

  unsigned long pulseDurationMicroseconds =
    receivedDocument[
      "pulseDurationMicroseconds"
    ] | 0UL;

  firebaseDocument[
    "pulseDurationMicroseconds"
  ] = pulseDurationMicroseconds;

  bool validSensorReading =
    strcmp(sensorStatus, "ok") == 0 &&
    !receivedDocument[
      "distanceMillimeters"
    ].isNull();

  if (validSensorReading) {
    float distanceMillimeters =
      receivedDocument[
        "distanceMillimeters"
      ];

    float distanceCentimeters =
      distanceMillimeters / 10.0f;

    float distanceMeters =
      distanceMillimeters / 1000.0f;

    firebaseDocument[
      "distanceMillimeters"
    ] = distanceMillimeters;

    firebaseDocument[
      "distanceCentimeters"
    ] = distanceCentimeters;

    firebaseDocument[
      "distanceMeters"
    ] = distanceMeters;

    if (
      SENSOR_TO_ZERO_WATER_LEVEL_MILLIMETERS >
      0.0f
    ) {
      float waterLevelMillimeters =
        SENSOR_TO_ZERO_WATER_LEVEL_MILLIMETERS -
        distanceMillimeters;

      if (waterLevelMillimeters < 0.0f) {
        waterLevelMillimeters = 0.0f;
      }

      firebaseDocument[
        "waterLevelMillimeters"
      ] = waterLevelMillimeters;

      firebaseDocument[
        "waterLevelCentimeters"
      ] = waterLevelMillimeters / 10.0f;

      firebaseDocument[
        "waterLevelMeters"
      ] = waterLevelMillimeters / 1000.0f;
    }
  } else {
    firebaseDocument[
      "distanceMillimeters"
    ] = nullptr;

    firebaseDocument[
      "distanceCentimeters"
    ] = nullptr;

    firebaseDocument[
      "distanceMeters"
    ] = nullptr;
  }

  firebaseDocument[
    "station1ToStation2RssiDbm"
  ] = receivedDocument[
    "relayReceivedRssiDbm"
  ] | 0;

  firebaseDocument[
    "station1ToStation2SnrDb"
  ] = receivedDocument[
    "relayReceivedSnrDb"
  ] | 0.0f;

  firebaseDocument[
    "station2ToStation3RssiDbm"
  ] = station2ToStation3RssiDbm;

  firebaseDocument[
    "station2ToStation3SnrDb"
  ] = station2ToStation3SnrDb;

  firebaseDocument[
    "station3UptimeMilliseconds"
  ] = millis();

  if (WiFi.status() == WL_CONNECTED) {
    firebaseDocument[
      "station3WifiRssiDbm"
    ] = WiFi.RSSI();
  } else {
    firebaseDocument[
      "station3WifiRssiDbm"
    ] = nullptr;
  }

  time_t currentUnixTime = time(nullptr);

  if (currentUnixTime > 1700000000) {
    firebaseDocument["timestampUnix"] =
      static_cast<int64_t>(
        currentUnixTime
      );

    firebaseDocument["timestampUtc"] =
      getUtcTimestamp();
  } else {
    firebaseDocument["timestampUnix"] =
      nullptr;

    firebaseDocument["timestampUtc"] =
      nullptr;
  }

  pendingFirebaseJson = "";

  serializeJson(
    firebaseDocument,
    pendingFirebaseJson
  );

  pendingSessionId = sessionId;
  pendingPacketId = packetId;
  hasPendingUpload = true;

  Serial.println("Firebase JSON prepared:");
  Serial.println(pendingFirebaseJson);
}

void receiveLoRaPacket() {
  int packetSize = LoRa.parsePacket();

  if (packetSize <= 0) {
    return;
  }

  String receivedJson =
    readLoRaPacket(packetSize);

  int station2ToStation3RssiDbm =
    LoRa.packetRssi();

  float station2ToStation3SnrDb =
    LoRa.packetSnr();

  LoRa.receive();

  Serial.println();
  Serial.println("================================");

  Serial.print("Received packet size: ");
  Serial.print(packetSize);
  Serial.println(" bytes");

  Serial.println("Received JSON:");
  Serial.println(receivedJson);

  Serial.print(
    "Station 2 to Station 3 RSSI: "
  );
  Serial.print(
    station2ToStation3RssiDbm
  );
  Serial.println(" dBm");

  Serial.print(
    "Station 2 to Station 3 SNR: "
  );
  Serial.print(
    station2ToStation3SnrDb,
    2
  );
  Serial.println(" dB");

  JsonDocument receivedDocument;

  DeserializationError jsonError =
    deserializeJson(
      receivedDocument,
      receivedJson
    );

  if (jsonError) {
    Serial.print("Invalid JSON: ");
    Serial.println(jsonError.c_str());
    return;
  }

  const char *sourceStation =
    receivedDocument[
      "sourceStation"
    ] | "";

  const char *destinationStation =
    receivedDocument[
      "destinationStation"
    ] | "";

  if (
    strcmp(sourceStation, "station1") != 0
  ) {
    Serial.println(
      "Invalid source station"
    );
    return;
  }

  if (
    strcmp(
      destinationStation,
      "station3"
    ) != 0
  ) {
    Serial.println(
      "Packet is not intended for Station 3"
    );
    return;
  }

  if (
    receivedDocument[
      "sessionId"
    ].isNull() ||
    receivedDocument[
      "packetId"
    ].isNull()
  ) {
    Serial.println(
      "Session ID or packet ID is missing"
    );
    return;
  }

  uint32_t receivedSessionId =
    receivedDocument["sessionId"];

  uint32_t receivedPacketId =
    receivedDocument["packetId"];

  if (
    hasPreviousPacket &&
    receivedSessionId ==
      previousSessionId &&
    receivedPacketId ==
      previousPacketId
  ) {
    Serial.println(
      "Duplicate packet ignored"
    );
    return;
  }

  prepareFirebaseJson(
    receivedDocument,
    station2ToStation3RssiDbm,
    station2ToStation3SnrDb
  );

  previousSessionId =
    receivedSessionId;

  previousPacketId =
    receivedPacketId;

  hasPreviousPacket = true;
}

bool uploadPendingDataToFirebase() {
  if (!hasPendingUpload) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  Serial.println();
  Serial.println("Uploading to Firebase...");

  bool latestUploadSuccessful =
    sendJsonToFirebase(
      "/waterLevel/latest",
      pendingFirebaseJson
    );

  if (!latestUploadSuccessful) {
    Serial.println(
      "Failed to upload latest data"
    );
    return false;
  }

  String historyPath =
    "/waterLevel/history/";

  historyPath += String(
    pendingSessionId
  );

  historyPath += "_";

  historyPath += String(
    pendingPacketId
  );

  bool historyUploadSuccessful =
    sendJsonToFirebase(
      historyPath,
      pendingFirebaseJson
    );

  if (!historyUploadSuccessful) {
    Serial.println(
      "Failed to upload history data"
    );
    return false;
  }

  hasPendingUpload = false;

  Serial.println(
    "Firebase upload successful"
  );

  Serial.print("Latest path: ");
  Serial.println(
    "/waterLevel/latest"
  );

  Serial.print("History path: ");
  Serial.println(historyPath);

  return true;
}

void setup() {
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("Station 3 starting");
  Serial.println(
    "LoRa receiver and Firebase gateway"
  );

  initializeLoRa();
  connectToWiFi();

  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.nist.gov"
  );

  Serial.println("Station 3 ready");
  Serial.println(
    "Waiting for Station 2..."
  );
}

void loop() {
  maintainWiFiConnection();

  if (
    hasPendingUpload &&
    WiFi.status() == WL_CONNECTED
  ) {
    uploadPendingDataToFirebase();
  }

  receiveLoRaPacket();

  delay(10);
}