#include <Arduino.h>

// 🔴 تحديد الـ Pins بناءً على التوصيل في المحاكاة 🔴
const int GRID_SWITCH_PIN = 14; // Slide Switch على Pin 14
const int POT_BATTERY_PIN = 34; // Potentiometer على Pin 34 (ADC)
const int LED_V2H_PIN     = 13;  // LED أخضر على Pin 2
const int LED_CHARGE_PIN  = 12; // LED أحمر على Pin 12

enum EVMode {
  EV_IDLE,          
  EV_SMART_CHARGE,  
  EV_FAST_CHARGE,   
  EV_V2H_DISCHARGE  
};

struct EVSystemState {
  float batterySOC;      
  bool isConnected;      
  bool isGridAvailable;  
  bool isPeakHour;       
  float homeLoadkW;      
};

EVMode controlEVPipeline(const EVSystemState &sys, String &explainableReason) {
  if (!sys.isConnected) {
    explainableReason = "🔌 EV Disconnected: No action possible.";
    return EV_IDLE;
  }

  // انقطاع الكهرباء -> V2H
  if (!sys.isGridAvailable) {
    if (sys.batterySOC > 20.0) { 
      explainableReason = "⚡ V2H ACTIVATED: Supplying home loads from EV battery (" + String(sys.batterySOC, 0) + "% SOC).";
      return EV_V2H_DISCHARGE;
    } else {
      explainableReason = "⚠️ V2H DISABLED: Battery too low (" + String(sys.batterySOC, 0) + "%) to discharge safely.";
      return EV_IDLE;
    }
  }

  // الكهرباء شغالة -> شحن
  if (sys.batterySOC < 15.0) {
    explainableReason = "🚀 FAST CHARGE: Battery critically low (" + String(sys.batterySOC, 0) + "%). Charging immediately!";
    return EV_FAST_CHARGE;
  }

  if (sys.isPeakHour && sys.batterySOC >= 80.0) {
    explainableReason = "⏸️ CHARGE PAUSED: Peak hours detected. Pausing to save cost (" + String(sys.batterySOC, 0) + "%).";
    return EV_IDLE;
  }

  if (sys.batterySOC < 95.0) {
    explainableReason = "🟢 SMART CHARGE: Charging EV efficiently (" + String(sys.batterySOC, 0) + "%).";
    return EV_SMART_CHARGE;
  }

  explainableReason = "✅ FULLY CHARGED: Battery full (" + String(sys.batterySOC, 0) + "%). Standby.";
  return EV_IDLE;
}

void setup() {
  Serial.begin(115200);
  
  pinMode(GRID_SWITCH_PIN, INPUT);
  pinMode(POT_BATTERY_PIN, INPUT);
  pinMode(LED_V2H_PIN, OUTPUT);
  pinMode(LED_CHARGE_PIN, OUTPUT);

  Serial.println("--- 🚗 EV & V2H Smart Simulator Ready ---");
}

void loop() {
  // 1. قراءة حالة الكهرباء (HIGH = شغالة / LOW = مقطوعة)
  bool gridAvailable = (digitalRead(GRID_SWITCH_PIN) == HIGH);

  // 2. قراءة البطارية من المقاومة
  int rawADC = analogRead(POT_BATTERY_PIN);
  float batterySOC = (rawADC / 4095.0) * 100.0;

  // 3. بناء حالة النظام
  EVSystemState currentState = {batterySOC, true, gridAvailable, false, 2.5};

  // 4. اتخاذ القرار من الموديول
  String reason = "";
  EVMode mode = controlEVPipeline(currentState, reason);

  // 5. تحديث اللمبات بناءً على الحالة
  if (mode == EV_V2H_DISCHARGE) {
    digitalWrite(LED_V2H_PIN, HIGH);   // ينور الأخضر
    digitalWrite(LED_CHARGE_PIN, LOW);  // يطفي الأحمر
  } 
  else if (mode == EV_SMART_CHARGE || mode == EV_FAST_CHARGE) {
    digitalWrite(LED_V2H_PIN, LOW);    // يطفي الأخضر
    digitalWrite(LED_CHARGE_PIN, HIGH); // ينور الأحمر
  } 
  else {
    digitalWrite(LED_V2H_PIN, LOW);
    digitalWrite(LED_CHARGE_PIN, LOW);
  }

  // 6. طباعة القرار
  Serial.println(reason);
  delay(1000);
}