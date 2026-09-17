#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>

// ==========================================================
// PIN DEFINITIONS
// ==========================================================
#define LDR_PIN       34    // ADC1 (GPIO 34) - Sunlight sensor
#define SOLAR_RELAY   21    // تم التبديل ليتطابق مع توصيل الماكيت الفعلي
#define GRID_RELAY    18    // تم التبديل ليتطابق مع توصيل الماكيت الفعلي

#define RELAY_ON      LOW
#define RELAY_OFF     HIGH

const int LIGHT_THRESHOLD = 300; // تم ضبطها على 300 لأن قراءة الحساس عندك بتوصل 545 // Above this = Sufficient Sunlight

bool currentLightState = false;
bool previousLightState = false;
bool firstReading = true;
int solarOverrideMode = 0; // 0: AUTO, 1: FORCE SOLAR, 2: FORCE GRID

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ==========================================================
// UNIFIED DATA STRUCTURE (Must match Master exactly)
// ==========================================================
typedef struct struct_message {
  int nodeID;           // 1: Solar, 2: Bedroom, 3: Kitchen, 4: Living Room
  bool gasAlarm;
  float batteryVolts;
  float sunlight;
  float insideTemp;
  float insideHumidity;
  float outsideTemp;
  bool occupied;
  int coolingMode;      // In Solar node: 1 = Solar Active, 0 = Grid Active
  int fanSpeed;
  int overrideMode;     // 0: AUTO, 1: FORCE SOLAR, 2: FORCE GRID
  
  // Living Room Controls (Node 4)
  bool led1;
  bool led2;
  bool tv;
  bool curtainOpen;
  int livingMode;
} SystemData;

SystemData solarData;
SystemData incomingCmd;

int readLDR() {
  long total = 0;
  const int samples = 15;
  for (int i = 0; i < samples; i++) {
    total += analogRead(LDR_PIN);
    delayMicroseconds(100);
  }
  return total / samples;
}

void solarMode() {
  digitalWrite(GRID_RELAY, RELAY_OFF);
  delay(50);
  digitalWrite(SOLAR_RELAY, RELAY_ON);
  currentLightState = true;
  Serial.println("[POWER SOURCE] Switched to SOLAR ENERGY");
}

void gridMode() {
  digitalWrite(SOLAR_RELAY, RELAY_OFF);
  delay(50);
  digitalWrite(GRID_RELAY, RELAY_ON);
  currentLightState = false;
  Serial.println("[POWER SOURCE] Switched to GRID / BATTERY");
}

#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingDataBuf, int len) {
#else
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingDataBuf, int len) {
#endif
  if (len != sizeof(SystemData)) return;
  memcpy(&incomingCmd, incomingDataBuf, sizeof(incomingCmd));

  if (incomingCmd.nodeID == 1) {
    solarOverrideMode = incomingCmd.overrideMode;
    Serial.print("[ESP-NOW] Solar Override Mode Received: ");
    Serial.println(solarOverrideMode);

    if (solarOverrideMode == 1) {
      solarMode();
    } else if (solarOverrideMode == 2) {
      gridMode();
    }
  }
}

void sendSolarTelemetry(int ldrVal, bool isSolar) {
  memset(&solarData, 0, sizeof(solarData));
  solarData.nodeID = 1; // Node 1: Solar & Microgrid
  solarData.sunlight = map(constrain(ldrVal, 0, 700), 0, 700, 0, 100); // 0 to 100%
  solarData.batteryVolts = isSolar ? 13.8 : 12.2;
  solarData.coolingMode = isSolar ? 1 : 0; // 1 = Solar, 0 = Grid
  solarData.overrideMode = solarOverrideMode;
  
  esp_now_send(broadcastAddress, (uint8_t *)&solarData, sizeof(solarData));
}

void setup() {
  Serial.begin(115200);

  pinMode(LDR_PIN, INPUT);
  pinMode(SOLAR_RELAY, OUTPUT);
  pinMode(GRID_RELAY, OUTPUT);

  digitalWrite(SOLAR_RELAY, RELAY_OFF);
  digitalWrite(GRID_RELAY, RELAY_OFF);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);
#else
  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);
#endif

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  }

  Serial.println("=== Solar & Microgrid Node (Node 1) Ready ===");
}

void loop() {
  int ldrValue = readLDR();

  // لو الوضع AUTO نتبع الحساس
  if (solarOverrideMode == 0) {
    bool detectedSun = (ldrValue > LIGHT_THRESHOLD);

    if (firstReading) {
      if (detectedSun) solarMode();
      else gridMode();
      previousLightState = detectedSun;
      firstReading = false;
    } else if (detectedSun != previousLightState) {
      if (detectedSun) solarMode();
      else gridMode();
      previousLightState = detectedSun;
    }
  }

  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    sendSolarTelemetry(ldrValue, currentLightState);

    Serial.print("LDR: ");
    Serial.print(ldrValue);
    Serial.print(" | Sun: ");
    Serial.print(map(constrain(ldrValue, 0, 700), 0, 700, 0, 100));
    Serial.print("% | Active: ");
    Serial.println(currentLightState ? "SOLAR" : "GRID/BATTERY");
  }

  delay(50);
}
