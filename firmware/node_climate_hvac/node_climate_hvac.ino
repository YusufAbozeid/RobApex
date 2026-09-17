#include <esp_now.h>
#include <WiFi.h>
#include <DHT.h>

// =====================================================
// PIN CONFIGURATION
// =====================================================

// Outside DHT
#define OUTSIDE_DHT_PIN 5

// Inside DHT
#define INSIDE_DHT_PIN 33

// PIR
#define PIR_PIN 32

// =====================================================
// L298N H-BRIDGE - SWAPPED CHANNELS
// =====================================================

// BLOWER (Channel A - LEFT SIDE)
#define BLOWER_ENA_PIN 12    // ENA
#define BLOWER_IN1_PIN 14    // IN1
#define BLOWER_IN2_PIN 27    // IN2

// FAN (Channel B - RIGHT SIDE)
#define FAN_ENB_PIN 13       // ENB
#define FAN_IN3_PIN 25       // IN3
#define FAN_IN4_PIN 26       // IN4

// =====================================================
// DHT TYPE & SETTINGS
// =====================================================
#define DHTTYPE DHT11

DHT outsideDHT(OUTSIDE_DHT_PIN, DHTTYPE);
DHT insideDHT(INSIDE_DHT_PIN, DHTTYPE);

const float COMFORT_TEMP = 20.0;
const float FAN_START_TEMP = 21.0;
const float STRONG_FAN_TEMP = 23.0;
const float BLOWER_TEMP = 24.0;

const float COMFORT_HUMIDITY = 55.0;
const float HIGH_HUMIDITY = 70.0;
const float VERY_HIGH_HUMIDITY = 80.0;

const int PWM_MAX = 255;

// PIR OCCUPANCY
const unsigned long OCCUPANCY_TIMEOUT = 10UL * 1000UL;
unsigned long lastMotionTime = 0;

enum CoolingMode {
  OFF = 0,
  FAN = 1,
  BLOWER = 2
};

CoolingMode currentMode = OFF;

// =====================================================
// ESP-NOW DATA STRUCTURE (Unified with Master)
// =====================================================
typedef struct struct_message {
  int nodeID;           // 2 for Climate Node
  bool gasAlarm;
  float batteryVolts;
  float sunlight;
  float insideTemp;
  float insideHumidity;
  float outsideTemp;
  bool occupied;
  int coolingMode;      // 0: OFF, 1: FAN, 2: BLOWER
  int fanSpeed;
  int overrideMode;     // 0: AUTO, 1: FORCE_FAN, 2: FORCE_BLOWER, 3: FORCE_OFF
  
  // Living Room Controls
  bool led1;
  bool led2;
  bool tv;
  bool curtainOpen;
  int livingMode;
} SystemData;

SystemData climateData;
SystemData incomingCmd;

// Master Broadcast Address
uint8_t masterAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int manualOverrideMode = 0; // 0: Auto, 1: Force Fan, 2: Force Blower, 3: Force Off
int currentAppliedSpeed = 0;

// =====================================================
// MOTOR CONTROL FUNCTIONS
// =====================================================
void fanStop() {
  analogWrite(FAN_ENB_PIN, 0);
  digitalWrite(FAN_IN3_PIN, LOW);
  digitalWrite(FAN_IN4_PIN, LOW);
}

void fanForward(int speed) {
  speed = constrain(speed, 0, PWM_MAX);
  digitalWrite(FAN_IN3_PIN, HIGH);
  digitalWrite(FAN_IN4_PIN, LOW);
  analogWrite(FAN_ENB_PIN, speed);
}

void blowerStop() {
  analogWrite(BLOWER_ENA_PIN, 0);
  digitalWrite(BLOWER_IN1_PIN, LOW);
  digitalWrite(BLOWER_IN2_PIN, LOW);
}

void blowerForward(int speed) {
  speed = constrain(speed, 0, PWM_MAX);
  digitalWrite(BLOWER_IN1_PIN, HIGH);
  digitalWrite(BLOWER_IN2_PIN, LOW);
  analogWrite(BLOWER_ENA_PIN, speed);
}

// =====================================================
// CALCULATE BASE SPEED
// =====================================================
int calculateBaseSpeed(float insideTemp, float insideHumidity) {
  int speed;

  if (insideTemp < FAN_START_TEMP) {
    speed = 0;
  }
  else if (insideTemp < 23.0) {
    speed = 70;
  }
  else if (insideTemp < 25.0) {
    speed = 100;
  }
  else if (insideTemp < 26.0) {
    speed = 130;
  }
  else if (insideTemp < 29.0) {
    speed = 160;
  }
  else if (insideTemp < 31.0) {
    speed = 200;
  }
  else {
    speed = 255;
  }

  if (insideHumidity > COMFORT_HUMIDITY) {
    speed += 15;
  }
  if (insideHumidity > HIGH_HUMIDITY) {
    speed += 25;
  }
  if (insideHumidity > VERY_HIGH_HUMIDITY) {
    speed += 35;
  }

  return constrain(speed, 0, PWM_MAX);
}

// =====================================================
// DETERMINE COOLING MODE
// =====================================================
CoolingMode determineMode(float insideTemp,
                            float insideHumidity,
                            float outsideTemp,
                            float outsideHumidity) {

  float tempDifference = insideTemp - outsideTemp;

  if (insideTemp <= COMFORT_TEMP && insideHumidity <= COMFORT_HUMIDITY) {
    return OFF;
  }
  if (insideHumidity >= VERY_HIGH_HUMIDITY) {
    return BLOWER;
  }
  if (insideTemp >= BLOWER_TEMP) {
    return BLOWER;
  }
  if (tempDifference >= 2.0 && outsideHumidity <= insideHumidity + 5.0) {
    return FAN;
  }
  if (insideTemp >= FAN_START_TEMP) {
    return FAN;
  }

  return OFF;
}

// =====================================================
// ESP-NOW RECEIVE CALLBACK
// =====================================================
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void OnDataRecv(const esp_now_recv_info_t * recv_info, const uint8_t *incomingDataBuf, int len) {
#else
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingDataBuf, int len) {
#endif
  if (len == sizeof(SystemData)) {
    memcpy(&incomingCmd, incomingDataBuf, sizeof(incomingCmd));
    
    if (incomingCmd.nodeID == 2) {
      manualOverrideMode = incomingCmd.overrideMode;
      Serial.print("Master Override Received: ");
      Serial.println(manualOverrideMode);
    }
  }
}

// =====================================================
// CONTROL COOLING
// =====================================================
void controlCooling(float insideTemp,
                    float insideHumidity,
                    float outsideTemp,
                    float outsideHumidity,
                    bool occupied) {

  // 1. التحكم اليدوي القادم من الماستر أولاً
  if (manualOverrideMode == 1) { // Force FAN
    currentMode = FAN;
    currentAppliedSpeed = 180;
    blowerStop();
    fanForward(currentAppliedSpeed);
    Serial.println("OVERRIDE: FORCE FAN");
    return;
  } 
  else if (manualOverrideMode == 2) { // Force BLOWER
    currentMode = BLOWER;
    currentAppliedSpeed = 255;
    fanStop();
    blowerForward(currentAppliedSpeed);
    Serial.println("OVERRIDE: FORCE BLOWER");
    return;
  } 
  else if (manualOverrideMode == 3) { // Force OFF
    currentMode = OFF;
    currentAppliedSpeed = 0;
    fanStop();
    blowerStop();
    Serial.println("OVERRIDE: FORCE OFF");
    return;
  }

  // 2. التحكم الأوتوماتيكي الأصلي
  if (!occupied) {
    Serial.println("ROOM EMPTY -> COOLING OFF");
    fanStop();
    blowerStop();
    currentMode = OFF;
    currentAppliedSpeed = 0;
    return;
  }

  CoolingMode newMode = determineMode(insideTemp, insideHumidity, outsideTemp, outsideHumidity);
  currentMode = newMode;

  if (currentMode == OFF) {
    Serial.println("MODE: OFF");
    fanStop();
    blowerStop();
    currentAppliedSpeed = 0;
    return;
  }

  int speed = calculateBaseSpeed(insideTemp, insideHumidity);

  if (currentMode == FAN) {
    speed = constrain(speed, 60, 170);
    if (outsideTemp < insideTemp - 3.0) speed += 20;
    if (outsideTemp > insideTemp + 2.0) speed -= 20;
    speed = constrain(speed, 50, 180);

    Serial.println("MODE: FAN");
    Serial.print("Fan speed: ");
    Serial.print(speed);
    Serial.println("/255");

    blowerStop();
    fanForward(speed);
    currentAppliedSpeed = speed;
    return;
  }

  if (currentMode == BLOWER) {
    speed = constrain(speed, 150, 255);
    if (insideHumidity >= VERY_HIGH_HUMIDITY) speed += 20;
    if (insideTemp >= 31.0) speed = 255;
    speed = constrain(speed, 150, 255);

    Serial.println("MODE: BLOWER / AC");
    Serial.print("Blower speed: ");
    Serial.print(speed);
    Serial.println("/255");

    fanStop();
    blowerForward(speed);
    currentAppliedSpeed = speed;
    return;
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("   SMART CLIMATE CONTROLLER (NODE 2)  ");
  Serial.println("======================================");

  // DHT
  outsideDHT.begin();
  insideDHT.begin();

  // PIR
  pinMode(PIR_PIN, INPUT_PULLDOWN);

  // L298N H-Bridge
  pinMode(BLOWER_ENA_PIN, OUTPUT);
  pinMode(BLOWER_IN1_PIN, OUTPUT);
  pinMode(BLOWER_IN2_PIN, OUTPUT);
  
  pinMode(FAN_ENB_PIN, OUTPUT);
  pinMode(FAN_IN3_PIN, OUTPUT);
  pinMode(FAN_IN4_PIN, OUTPUT);

  fanStop();
  blowerStop();

  lastMotionTime = millis() - OCCUPANCY_TIMEOUT - 1;

  // ESP-NOW SETUP
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0; // متوافق مع الماستر
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
  }

  Serial.println("System & ESP-NOW Initialized.");
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  bool motionDetected = digitalRead(PIR_PIN);
  if (motionDetected) {
    lastMotionTime = millis();
  }
  bool occupied = motionDetected || ((millis() - lastMotionTime) < OCCUPANCY_TIMEOUT);

  static unsigned long lastSensorRead = 0;
  static float outsideTemp = 24.0, outsideHumidity = 50.0;
  static float insideTemp = 25.0, insideHumidity = 55.0;

  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();

    float oT = outsideDHT.readTemperature();
    float oH = outsideDHT.readHumidity();
    float iT = insideDHT.readTemperature();
    float iH = insideDHT.readHumidity();

    if (!isnan(oT) && !isnan(oH) && !isnan(iT) && !isnan(iH)) {
      outsideTemp = oT;
      outsideHumidity = oH;
      insideTemp = iT;
      insideHumidity = iH;
    }

    // إرسال البيانات للـ Master ESP32
    memset(&climateData, 0, sizeof(climateData));
    climateData.nodeID = 2;
    climateData.insideTemp = insideTemp;
    climateData.insideHumidity = insideHumidity;
    climateData.outsideTemp = outsideTemp;
    climateData.occupied = occupied;
    climateData.coolingMode = (int)currentMode;
    climateData.fanSpeed = currentAppliedSpeed;
    climateData.overrideMode = manualOverrideMode;

    esp_now_send(masterAddress, (uint8_t *)&climateData, sizeof(climateData));
  }

  // تطبيق التحكم بالمحركات لحظياً (سريع الاستجابة لأوامر الداشبورد)
  controlCooling(insideTemp, insideHumidity, outsideTemp, outsideHumidity, occupied);

  delay(50);
}
