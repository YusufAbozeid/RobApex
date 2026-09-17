/* =========================================================
   نظام الإضاءة الذكية - EcoMind AI OS (جزء يوسف عادل)
   النسخة الكاملة:
   - Multi-Zone (زونين مستقلين)
   - قياس إضاءة تناظري (Analog) + تعتيم متدرج حقيقي
   - محاكاة الوقت من اليوم (Simulated Time)
   - تفضيلات المستخدم (Preferences)
   - وضع تشغيل عام للمبنى (Building Mode) - Placeholder للدمج مع الفريق
   - مهلة إطفاء تلقائي + شرح القرارات (Explainable AI)
   ========================================================= */

#define NUM_ZONES 2

/* ------------------- تعريف البنات (Pins) ------------------- */
int pirPins[NUM_ZONES]   = { 0, 25 };   // خرج حساس الحركة لكل زون
int relayPins[NUM_ZONES] = { 26, 33 };  // إشارة PWM للريلاي/LED لكل زون
int lightAnalogPin = 34;                // حساس الإضاءة التناظري (AO) - GPIO34 لازم يكون ADC1

/* ------------------- إعدادات منطق القرار ------------------- */
#define OFF_DELAY_MS       5000   // مهلة الإطفاء التلقائي (زودها لاحقًا لقيمة واقعية)
#define TARGET_LUX         2500   // "الإضاءة الكلية" المطلوبة في المكان (قيمة ADC مرجعية 0-4095)

/* ------------------- إعدادات PWM ------------------- */
#define PWM_FREQ           5000
#define PWM_RESOLUTION     8      // قيم 0-255
#define BRIGHTNESS_OFF     0

/* ------------------- تفضيلات المستخدم (Placeholder بسيط) ------------------- */
int preferredMaxBrightness = 80;  // % - أقصى سطوع يفضله المستخدم (بدل 100% دايمًا)

/* ------------------- محاكاة الوقت من اليوم ------------------- */
// بما إننا لسه في Simulation من غير RTC/NTP فعلي، بنتحكم في الوقت يدويًا هنا للتجربة
// لما نوصل النت على الهاردوير الحقيقي، نستبدلها بـ NTP يجيب الساعة الحقيقية
int simulatedHour = 20; // مثال: الساعة 8 مساءً - غيّرها يدويًا لتجربة سيناريوهات مختلفة (0-23)

/* ------------------- وضع التشغيل العام للمبنى (Placeholder) ------------------- */
// ده المتغير اللي هيوصل بيه زميلك المسؤول عن نظام الطاقة/الأوضاع العامة بعدين
// 0 = Normal | 1 = EnergySaving | 2 = Sleep | 3 = Emergency
int buildingMode = 3;

/* ------------------- متغيرات الحالة لكل زون ------------------- */
unsigned long lastMotionTime[NUM_ZONES] = { 0, 0 };
bool zoneLightIsOn[NUM_ZONES] = { false, false };

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(pirPins[i], INPUT);
    ledcAttach(relayPins[i], PWM_FREQ, PWM_RESOLUTION);
    ledcWrite(relayPins[i], BRIGHTNESS_OFF);
  }

  Serial.println("=== Smart Multi-Zone Lighting System (Full Version) Started ===");
}

void explainDecision(int zone, const char* decision, String reason) {
  Serial.print("[Zone ");
  Serial.print(zone + 1);
  Serial.print("] [Decision] ");
  Serial.print(decision);
  Serial.print(" | [Reason] ");
  Serial.println(reason);
}

/* حساب أقصى سطوع مسموح به حسب وضع المبنى العام */
int getModeMaxBrightness() {
  switch (buildingMode) {
    case 1: return 128; // EnergySaving -> يقفل السقف عند 50%
    case 2: return 40;  // Sleep -> إضاءة خافتة جدًا بس
    case 3: return 255; // Emergency -> يسمح بأقصى سطوع (أولوية للسلامة)
    default: return 255; // Normal
  }
}

void loop() {
  int lightRaw = analogRead(lightAnalogPin); // قيمة من 0 إلى 4095

  unsigned long now = millis();

  Serial.print("[Sensors] Light Raw: ");
  Serial.print(lightRaw);
  Serial.print(" | Hour: ");
  Serial.print(simulatedHour);
  Serial.print(" | Building Mode: ");
  Serial.println(buildingMode);

  for (int i = 0; i < NUM_ZONES; i++) {
    int occupancy = digitalRead(pirPins[i]);

    if (occupancy == 1) {
      lastMotionTime[i] = now;
    }

    bool motionRecent = (now - lastMotionTime[i]) < OFF_DELAY_MS;

    Serial.print("Zone ");
    Serial.print(i + 1);
    Serial.print(" -> Occupancy: ");
    Serial.println(occupancy);

    if (!motionRecent) {
      /* مفيش حد -> اطفي */
      if (zoneLightIsOn[i]) {
        explainDecision(i, "Light OFF", "No motion detected for the configured timeout period");
      }
      ledcWrite(relayPins[i], BRIGHTNESS_OFF);
      zoneLightIsOn[i] = false;
      continue;
    }

    /* فيه حركة -> احسب السطوع المطلوب */
    int neededArtificial = TARGET_LUX - lightRaw;
    if (neededArtificial < 0) neededArtificial = 0;

    // حوّل القيمة لنسبة مئوية من 0-100 بناءً على الحد الأقصى TARGET_LUX
    int brightnessPercent = map(neededArtificial, 0, TARGET_LUX, 0, 100);
    brightnessPercent = constrain(brightnessPercent, 0, 100);

    // طبّق تفضيل المستخدم (سقف أقصى)
    if (brightnessPercent > preferredMaxBrightness) {
      brightnessPercent = preferredMaxBrightness;
    }

    // طبّق سقف وضع المبنى العام
    int modeMax = getModeMaxBrightness();
    int modeMaxPercent = map(modeMax, 0, 255, 0, 100);
    if (brightnessPercent > modeMaxPercent) {
      brightnessPercent = modeMaxPercent;
    }

    // تعديل بسيط حسب الوقت: بالليل (بعد 8 مساءً وقبل 6 صباحًا) نرفع الحد الأدنى شوية للراحة البصرية
    bool isNightTime = (simulatedHour >= 20 || simulatedHour < 6);
    if (isNightTime && brightnessPercent > 0 && brightnessPercent < 30) {
      brightnessPercent = 30;
    }

    int pwmValue = map(brightnessPercent, 0, 100, 0, 255);
    ledcWrite(relayPins[i], pwmValue);

    if (brightnessPercent > 0) {
      zoneLightIsOn[i] = true;
      String reason = "Motion detected, natural light needs supplement, mode=" + String(buildingMode) +
                       ", night=" + String(isNightTime ? "yes" : "no");
      String decisionText = "Light ON (" + String(brightnessPercent) + "%)";
      explainDecision(i, decisionText.c_str(), reason);
    } else {
      zoneLightIsOn[i] = false;
      explainDecision(i, "Light OFF", "Natural light is sufficient for this zone");
    }
  }

  Serial.println("---------------------------------------");
  delay(1000);
}