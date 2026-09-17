#include <WiFi.h>
#include <esp_now.h>

// ==========================================================
// PIN DEFINITIONS
// ==========================================================
#define GAS_SENSOR_PIN 35   // سلك D0 من الحساس (GPIO 35)
#define BUZZER_PIN     25   // سلك البزر (GPIO 25)

// ==========================================================
// THRESHOLDS & TIMINGS
// ==========================================================
#define GAS_THRESHOLD  1800 

// عنوان البث الموحد للماستر
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
  int coolingMode;
  int fanSpeed;
  int overrideMode;
  
  // Living Room Controls (Node 4)
  bool led1;
  bool led2;
  bool tv;
  bool curtainOpen;
  int livingMode;
} SystemData;

SystemData gasData;
esp_now_peer_info_t peerInfo;

void setup() {
  Serial.begin(115200);

  pinMode(GAS_SENSOR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // البزر مغلق في البداية
  digitalWrite(BUZZER_PIN, LOW); 

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  }

  Serial.println("=== Kitchen Safety Node (Node 3) Ready ===");
}

void loop() {
  int gasLevel = analogRead(GAS_SENSOR_PIN);

  // تصحيح الانعكاس:
  // لأن السلك من طرف D0: في الجو العادي يعطي 4095، ولما يلقط غاز يهبط للصفر
  // إذن التسريب يحدث عندما تكون القراءة أقل من 1800 (< وليس >=)
  bool isGasLeaking = (gasLevel < GAS_THRESHOLD);

  if (isGasLeaking) {
    // إنذار صوتي متقطع عند الغاز
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
    Serial.print("⚠️ CRITICAL ALERT: Gas Leak Detected! Raw Level: ");
    Serial.println(gasLevel);
  } else {
    // البزر ساكت تماماً في الجو العادي
    digitalWrite(BUZZER_PIN, LOW);
  }

  // إرسال البيانات للماستر كل 1 ثانية
  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 1000) {
    lastSend = millis();

    memset(&gasData, 0, sizeof(gasData));
    gasData.nodeID = 3; // نود 3 للمطبخ
    gasData.gasAlarm = isGasLeaking;

    esp_now_send(broadcastAddress, (uint8_t *)&gasData, sizeof(gasData));
  }

  delay(50);
}
