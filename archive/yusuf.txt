#include <esp_idf_version.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// حماية متقدمة لمنع ريستارت البوردة عند تشغيل المحرك والريلايات
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================================
// PIN DEFINITIONS (تطابق كامل 100% مع التوصيل الفعلي للماكيت)
// ==========================================================
// 1. نظام الستارة (Curtain Motor - Relay 2)
const int LDR_PIN = 34;    // حساس الإضاءة المحلي ADC1 (GPIO 34) - اختياري
const int IN1_PIN = 5;     // ريلاي الستارة القناة الأولى رايح لـ D5
const int IN2_PIN = 18;    // ريلاي الستارة القناة الثانية رايح لـ D18

// 2. نظام التلفزيون والإضاءة وحساسات الحركة (Lighting, TV & PIR)
const int PIR1_PIN = 32;   // حساس الحركة اليمين (Zone 1) رايح لـ D32
const int PIR2_PIN = 33;   // حساس الحركة الشمال (Zone 2) رايح لـ D33
const int LED1_PIN = 25;   // ليد المنطقة اليمين رايح لـ D25
const int LED2_PIN = 26;   // ليد المنطقة الشمال رايح لـ D26
const int TV_PIN   = 27;   // ريلاي التلفزيون (IN2 في ريلاي 1) رايح لـ D27

// تعريف تشغيل وإطفاء الريلاي (Active LOW)
#define TV_RELAY_ON   LOW
#define TV_RELAY_OFF  HIGH

// 3. شاشة التلفزيون الذكية OLED TV Screen (I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_SDA      21   // SDA رايح لـ D21
#define OLED_SCL      22   // SCL رايح لـ D22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = false;

// ==========================================================
// TIMINGS & THRESHOLDS
// ==========================================================
const unsigned long OCCUPANCY_TIMEOUT = 12000UL; // 12 ثانية بعد توقف الحركة لإطفاء الأنوار والتلفزيون
const unsigned long MOTOR_RUN_TIME = 1500UL;     // مدة دوران محرك الستارة (1.5 ثانية)

// الاعتماد على حساس الطاقة الشمسية (Node 1 - Reem) عبر شبكة ESP-NOW المشتركة
#define USE_SOLAR_NODE_LDR true

int SUNNY_THRESHOLD = 400; // عتبة الضوء للحساس المحلي (لو تم استخدامه)
int DARK_THRESHOLD  = 150; // عتبة الظلام للحساس المحلي (لو تم استخدامه)

// ==========================================================
// SYSTEM STATES
// ==========================================================
enum CurtainState { CURTAIN_OPEN, CURTAIN_CLOSED, CURTAIN_IDLE };
CurtainState currentCurtainState = CURTAIN_IDLE;

bool isMotorRunning = false;
unsigned long motorStartTime = 0;

unsigned long lastMotionTime1 = 0;
unsigned long lastMotionTime2 = 0;

bool isAutoMode = true;   // true = أوتوماتيك بالحساسات, false = تحكم يدوي من الماستر
bool led1State = false;
bool led2State = false;
bool tvState   = false;

// بيانات الطاقة الشمسية المستلمة من نود 1 (Reem)
float solarSunlight = 0.0;
bool solarIsActive = false;
bool hasSolarData = false;

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
  int livingMode;       // 0: AUTO, 1: MANUAL
} SystemData;

SystemData roomData;
SystemData incomingCmd;

// ==========================================================
// OLED DISPLAY FUNCTIONS (TV Screen Graphics)
// ==========================================================
void updateOLED(bool tvOn, bool l1, bool l2) {
  if (!oledAvailable) return;

  display.clearDisplay();
  
  if (tvOn) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    display.drawRect(2, 2, 124, 60, SSD1306_WHITE);
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 8);
    display.print("SMART TV - ACTIVE");
    
    display.drawLine(10, 20, 118, 20, SSD1306_WHITE);
    display.setCursor(10, 26);
    display.print("CH: 4K SCIENCE LIVE");
    
    display.setCursor(10, 38);
    display.print("ENERGY: ECO MODE");

    display.drawCircle(112, 12, 3, SSD1306_WHITE);
    display.drawCircle(112, 12, 6, SSD1306_WHITE);

    display.setCursor(10, 50);
    display.print("ZONE: ");
    display.print(l1 ? "R " : "- ");
    display.print(l2 ? "L" : "-");
  } else {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(12, 18);
    display.print("[ ZERO STANDBY ]");
    display.setCursor(18, 34);
    display.print("TV POWER: 0.0W");
    display.setCursor(14, 48);
    display.print("ROOM IS VACANT");
  }
  
  display.display();
}

// ==========================================================
// MOTOR CONTROL (مع مهلة أمان Dead-Time لمنع القفلة والشرز)
// ==========================================================
void stopMotor() {
  digitalWrite(IN1_PIN, HIGH); // Relays OFF (Active LOW)
  digitalWrite(IN2_PIN, HIGH);
  isMotorRunning = false;
  Serial.println("[Motor] Stopped.");
}

void openCurtain() {
  if (currentCurtainState == CURTAIN_OPEN && !isMotorRunning) return;
  Serial.println("[Curtain] Opening...");
  digitalWrite(IN2_PIN, HIGH);  // التأكد من إطفاء ريلاي 2 أولاً
  delay(30);                    // مهلة أمان كهروميكانيكية (Dead-Time)
  digitalWrite(IN1_PIN, LOW);   // تشغيل ريلاي 1 (D5)
  motorStartTime = millis();
  isMotorRunning = true;
  currentCurtainState = CURTAIN_OPEN;
}

void closeCurtain() {
  if (currentCurtainState == CURTAIN_CLOSED && !isMotorRunning) return;
  Serial.println("[Curtain] Closing...");
  digitalWrite(IN1_PIN, HIGH);  // التأكد من إطفاء ريلاي 1 أولاً
  delay(30);                    // مهلة أمان كهروميكانيكية (Dead-Time)
  digitalWrite(IN2_PIN, LOW);   // تشغيل ريلاي 2 (D18)
  motorStartTime = millis();
  isMotorRunning = true;
  currentCurtainState = CURTAIN_CLOSED;
}

void processMotorTimer() {
  if (isMotorRunning && (millis() - motorStartTime >= MOTOR_RUN_TIME)) {
    stopMotor();
  }
}

// ==========================================================
// ESP-NOW CALLBACKS
// ==========================================================
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingDataBuf, int len) {
#else
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingDataBuf, int len) {
#endif
  if (len != sizeof(SystemData)) return;
  memcpy(&incomingCmd, incomingDataBuf, sizeof(incomingCmd));

  // 1. استقبال بيانات الطاقة الشمسية من ريم (Node 1 - Solar Microgrid)
  if (incomingCmd.nodeID == 1) {
    solarSunlight = incomingCmd.sunlight;
    solarIsActive = (incomingCmd.coolingMode == 1); // 1 = كافي للشمس (نهار), 0 = ليل / بطارية
    hasSolarData = true;

    #if USE_SOLAR_NODE_LDR
    if (isAutoMode && !isMotorRunning) {
      if (solarIsActive && currentCurtainState != CURTAIN_OPEN) {
        Serial.print("[SOLAR SYNC] Daylight Detected (");
        Serial.print(solarSunlight);
        Serial.println("%) -> Opening Curtain!");
        openCurtain();
      } else if (!solarIsActive && currentCurtainState != CURTAIN_CLOSED) {
        Serial.print("[SOLAR SYNC] Darkness / Night (");
        Serial.print(solarSunlight);
        Serial.println("%) -> Closing Curtain!");
        closeCurtain();
      }
    }
    #endif
  }

  // 2. استقبال أوامر الماستر للغرفة (Node 4)
  if (incomingCmd.nodeID == 4) {
    isAutoMode = (incomingCmd.livingMode == 0);
    Serial.print("[ESP-NOW] Living Mode Set: ");
    Serial.println(isAutoMode ? "AUTO" : "MANUAL");

    if (incomingCmd.curtainOpen && currentCurtainState != CURTAIN_OPEN) {
      openCurtain();
    } else if (!incomingCmd.curtainOpen && currentCurtainState != CURTAIN_CLOSED) {
      closeCurtain();
    }

    if (!isAutoMode) {
      led1State = incomingCmd.led1;
      led2State = incomingCmd.led2;
      tvState = incomingCmd.tv;

      digitalWrite(LED1_PIN, led1State ? HIGH : LOW);
      digitalWrite(LED2_PIN, led2State ? HIGH : LOW);
      digitalWrite(TV_PIN, tvState ? TV_RELAY_ON : TV_RELAY_OFF);
      updateOLED(tvState, led1State, led2State);
    }
  }
}

void sendTelemetry() {
  memset(&roomData, 0, sizeof(roomData));
  roomData.nodeID = 4;
  roomData.led1 = led1State;
  roomData.led2 = led2State;
  roomData.tv = tvState;
  roomData.curtainOpen = (currentCurtainState == CURTAIN_OPEN);
  roomData.livingMode = isAutoMode ? 0 : 1;
  roomData.occupied = (led1State || led2State);

  esp_now_send(broadcastAddress, (uint8_t *)&roomData, sizeof(roomData));
}

// ==========================================================
// AUTOMATIC SENSOR LOGIC
// ==========================================================
void handleSensors() {
  if (!isAutoMode) return;

  unsigned long currentMillis = millis();

  int motionRight = digitalRead(PIR1_PIN);
  int motionLeft  = digitalRead(PIR2_PIN);

  if (motionRight == HIGH) {
    lastMotionTime1 = currentMillis;
    if (!led1State) Serial.println(">>> [PIR 1] Motion Detected (RIGHT) -> LED 1 ON <<<");
    led1State = true;
  } else if (currentMillis - lastMotionTime1 > OCCUPANCY_TIMEOUT) {
    if (led1State) Serial.println("[PIR 1] Timeout reached -> LED 1 OFF");
    led1State = false;
  }

  if (motionLeft == HIGH) {
    lastMotionTime2 = currentMillis;
    if (!led2State) Serial.println(">>> [PIR 2] Motion Detected (LEFT) -> LED 2 ON <<<");
    led2State = true;
  } else if (currentMillis - lastMotionTime2 > OCCUPANCY_TIMEOUT) {
    if (led2State) Serial.println("[PIR 2] Timeout reached -> LED 2 OFF");
    led2State = false;
  }

  digitalWrite(LED1_PIN, led1State ? HIGH : LOW);
  digitalWrite(LED2_PIN, led2State ? HIGH : LOW);

  // TV Logic: إذا انطفأت كل الليدات (الغرفة خالية) -> إطفاء فوري للتلفزيون وعزل الحمل تماماً
  bool shouldTvBeOn = (led1State || led2State);

  if (shouldTvBeOn != tvState) {
    tvState = shouldTvBeOn;
    digitalWrite(TV_PIN, tvState ? TV_RELAY_ON : TV_RELAY_OFF);
    updateOLED(tvState, led1State, led2State);
    Serial.println(tvState ? ">>> [TV RELAY] ACTIVE (Occupied) <<<" : ">>> [TV RELAY] SHUTDOWN (Vacant - 0W Phantom Load) <<<");
  }

  // إذا لم نكن نعتمد على حساس السولار الخارجي، نعتمد على الحساس المحلي
  #if !USE_SOLAR_NODE_LDR
  int ldrValue = analogRead(LDR_PIN);
  if (ldrValue > SUNNY_THRESHOLD && currentCurtainState != CURTAIN_OPEN && !isMotorRunning) {
    Serial.print("[LDR: "); Serial.print(ldrValue); Serial.println("] Sunlight detected -> Opening Curtain");
    openCurtain();
  } else if (ldrValue < DARK_THRESHOLD && currentCurtainState != CURTAIN_CLOSED && !isMotorRunning) {
    Serial.print("[LDR: "); Serial.print(ldrValue); Serial.println("] Darkness detected -> Closing Curtain");
    closeCurtain();
  }
  #endif
}

// ==========================================================
// SETUP
// ==========================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(500);

  Serial.println("
==================================================");
  Serial.println("   RoboDam Living Room Controller (Node 4) Ready  ");
  Serial.println("==================================================");

  // 1. تهيئة محرك الستارة
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  stopMotor();

  #if !USE_SOLAR_NODE_LDR
  pinMode(LDR_PIN, INPUT);
  #endif

  // 2. تهيئة حساسات الحركة والليدات وريلاي التلفزيون
  pinMode(PIR1_PIN, INPUT);
  pinMode(PIR2_PIN, INPUT);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(TV_PIN, OUTPUT);

  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  digitalWrite(TV_PIN, TV_RELAY_OFF);

  // 3. تهيئة شاشة التلفزيون OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setTimeOut(100);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledAvailable = true;
    display.clearDisplay();
    display.display();
    updateOLED(false, false, false);
    Serial.println("[OLED] TV Screen Initialized (0x3C)");
  } else if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledAvailable = true;
    display.clearDisplay();
    display.display();
    updateOLED(false, false, false);
    Serial.println("[OLED] TV Screen Initialized (0x3D)");
  } else {
    Serial.println("[OLED] Warning: Screen not detected on 0x3C/0x3D, continuing without OLED.");
  }

  // 4. تهيئة شبكة ESP-NOW
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

  Serial.println("=== Living Room Smart Controller (Node 4) Running ===");
  Serial.println("[SYNC] Listening to Solar Node 1 for Natural Daylight Harvesting...");
}

// ==========================================================
// LOOP
// ==========================================================
void loop() {
  processMotorTimer();
  handleSensors();

  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    sendTelemetry();

    #if USE_SOLAR_NODE_LDR
    Serial.print("Solar Sun: ");
    if (hasSolarData) {
      Serial.print(solarSunlight);
      Serial.print("% [");
      Serial.print(solarIsActive ? "DAY/OPEN" : "NIGHT/CLOSED");
      Serial.print("]");
    } else {
      Serial.print("Waiting for Node 1...");
    }
    #else
    Serial.print("Local LDR: ");
    Serial.print(analogRead(LDR_PIN));
    #endif

    Serial.print(" | Curtain: ");
    Serial.print(currentCurtainState == CURTAIN_OPEN ? "OPEN" : (currentCurtainState == CURTAIN_CLOSED ? "CLOSED" : "IDLE"));
    Serial.print(" | PIR1(R): ");
    Serial.print(digitalRead(PIR1_PIN));
    Serial.print(" | PIR2(L): ");
    Serial.print(digitalRead(PIR2_PIN));
    Serial.print(" | LED-R: ");
    Serial.print(led1State ? "ON" : "OFF");
    Serial.print(" | LED-L: ");
    Serial.print(led2State ? "ON" : "OFF");
    Serial.print(" | TV: ");
    Serial.println(tvState ? "ACTIVE (ON)" : "STANDBY (0W)");
  }

  delay(20);
}
