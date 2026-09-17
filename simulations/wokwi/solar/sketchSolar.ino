#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHTesp.h>

// =====================================================
// OLED
// =====================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// =====================================================
// PINS
// =====================================================
const int LDR_PIN = 34;
const int DHT_PIN = 15;

const int CHARGE_LED = 25;
const int DISCHARGE_LED = 26;
const int STATUS_LED = 27;

const int BUZZER_PIN = 14;

// =====================================================
// SOLAR SYSTEM
// =====================================================

// Maximum simulated solar power
const float MAX_SOLAR_POWER_W = 300.0f;

// Solar system conversion efficiency
const float SOLAR_SYSTEM_EFFICIENCY = 0.90f;

// Solar panels lose some performance
// when temperature rises above 25 C
const float TEMP_COEFFICIENT = 0.004f;

// =====================================================
// BATTERY
// =====================================================
const float BATTERY_CAPACITY_WH = 1000.0f;

const float MIN_SOC = 20.0f;
const float MAX_SOC = 95.0f;

const float MAX_CHARGE_POWER_W = 120.0f;
const float MAX_DISCHARGE_POWER_W = 120.0f;

const float CHARGE_EFFICIENCY = 0.95f;
const float DISCHARGE_EFFICIENCY = 0.92f;

// Initial battery level
float batterySOC = 60.0f;

// Only speeds up battery movement in Wokwi
const float SIMULATION_SPEED = 300.0f;

// =====================================================
// HOME LOAD
// =====================================================

// Constant devices
const float FRIDGE_W = 40.0f;
const float ROUTER_W = 10.0f;
const float STANDBY_W = 10.0f;

// Variable loads
const float FAN_W = 30.0f;
const float AC_W = 80.0f;

// =====================================================
// CONTROL SETTINGS
// =====================================================
const float MIN_POWER_DIFFERENCE_W = 5.0f;

const int ADC_MAX = 4095;
const int ADC_SAMPLES = 10;

// =====================================================
// DHT22
// =====================================================
DHTesp dht;

// =====================================================
// SYSTEM VARIABLES
// =====================================================
float sunlightPercent = 0.0f;

float temperature = 0.0f;
float humidity = 0.0f;

float solarPower = 0.0f;

float homeLoad = 0.0f;

float solarToHome = 0.0f;
float solarToBattery = 0.0f;

float batteryToHome = 0.0f;

float gridToHome = 0.0f;

float excessSolar = 0.0f;

float solarSelfConsumption = 0.0f;
float solarCoverage = 0.0f;
float gridSaving = 0.0f;

String mode = "IDLE";

unsigned long previousTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastSerialTime = 0;

// =====================================================
// HELPERS
// =====================================================
float limitFloat(
  float value,
  float minimum,
  float maximum
) {
  if (value < minimum) {
    return minimum;
  }

  if (value > maximum) {
    return maximum;
  }

  return value;
}

float minimumFloat(float a, float b) {
  return (a < b) ? a : b;
}

float maximumFloat(float a, float b) {
  return (a > b) ? a : b;
}

// =====================================================
// ADC FILTER
// =====================================================
int readAverageADC(int pin) {

  long total = 0;

  for (int i = 0; i < ADC_SAMPLES; i++) {
    total += analogRead(pin);
  }

  return total / ADC_SAMPLES;
}

// =====================================================
// READ SUNLIGHT
// =====================================================
void readSunlight() {

  int raw =
    readAverageADC(LDR_PIN);

  // Wokwi LDR works inversely:
  // stronger light = smaller ADC
  sunlightPercent =
    (
      ADC_MAX - raw
    )
    /
    4095.0f
    *
    100.0f;

  sunlightPercent =
    limitFloat(
      sunlightPercent,
      0.0f,
      100.0f
    );
}

// =====================================================
// READ WEATHER
// =====================================================
void readWeather() {

  TempAndHumidity climate =
    dht.getTempAndHumidity();

  if (!isnan(climate.temperature)) {
    temperature =
      climate.temperature;
  }

  if (!isnan(climate.humidity)) {
    humidity =
      climate.humidity;
  }
}

// =====================================================
// SOLAR GENERATION
// =====================================================
void calculateSolarPower() {

  float sunlightFactor =
    sunlightPercent /
    100.0f;

  float temperatureEfficiency =
    1.0f;

  // Solar panel temperature loss
  if (temperature > 25.0f) {

    temperatureEfficiency =
      1.0f -
      (
        temperature - 25.0f
      )
      *
      TEMP_COEFFICIENT;
  }

  temperatureEfficiency =
    limitFloat(
      temperatureEfficiency,
      0.75f,
      1.0f
    );

  solarPower =
    MAX_SOLAR_POWER_W
    *
    sunlightFactor
    *
    SOLAR_SYSTEM_EFFICIENCY
    *
    temperatureEfficiency;
}

// =====================================================
// AUTOMATIC HOME LOAD
// =====================================================
void calculateHomeLoad() {

  // Basic appliances always working
  homeLoad =
    FRIDGE_W +
    ROUTER_W +
    STANDBY_W;

  // Moderate temperature
  if (
    temperature >= 25.0f &&
    temperature < 30.0f
  ) {
    homeLoad += FAN_W;
  }

  // Hot weather
  else if (
    temperature >= 30.0f
  ) {
    homeLoad += AC_W;
  }

  /*
     Result examples:

     Cool:
     60 W

     Medium:
     90 W

     Hot:
     140 W
  */
}

// =====================================================
// RESET OUTPUTS
// =====================================================
void resetOutputs() {

  digitalWrite(
    CHARGE_LED,
    LOW
  );

  digitalWrite(
    DISCHARGE_LED,
    LOW
  );

  digitalWrite(
    STATUS_LED,
    LOW
  );
}

// =====================================================
// ENERGY MANAGEMENT
// =====================================================
void manageEnergy() {

  resetOutputs();

  solarToHome = 0.0f;
  solarToBattery = 0.0f;
  batteryToHome = 0.0f;
  gridToHome = 0.0f;
  excessSolar = 0.0f;

  // Solar always supplies home first
  solarToHome =
    minimumFloat(
      solarPower,
      homeLoad
    );

  float difference =
    solarPower -
    homeLoad;

  // ===================================================
  // SOLAR > HOME
  // ===================================================
  if (
    difference >
    MIN_POWER_DIFFERENCE_W
  ) {

    float extraSolar =
      difference;

    // Battery not full
    if (
      batterySOC <
      MAX_SOC
    ) {

      mode =
        "CHARGING";

      solarToBattery =
        minimumFloat(
          extraSolar,
          MAX_CHARGE_POWER_W
        );

      excessSolar =
        maximumFloat(
          extraSolar -
          solarToBattery,
          0.0f
        );

      digitalWrite(
        CHARGE_LED,
        HIGH
      );
    }

    // Battery full
    else {

      mode =
        "BATTERY FULL";

      excessSolar =
        extraSolar;

      digitalWrite(
        STATUS_LED,
        HIGH
      );
    }
  }

  // ===================================================
  // SOLAR < HOME
  // ===================================================
  else if (
    difference <
    -MIN_POWER_DIFFERENCE_W
  ) {

    float deficit =
      -difference;

    // Battery available
    if (
      batterySOC >
      MIN_SOC
    ) {

      mode =
        "DISCHARGING";

      batteryToHome =
        minimumFloat(
          deficit,
          MAX_DISCHARGE_POWER_W
        );

      gridToHome =
        maximumFloat(
          deficit -
          batteryToHome,
          0.0f
        );

      digitalWrite(
        DISCHARGE_LED,
        HIGH
      );
    }

    // Battery too low
    else {

      mode =
        "USE GRID";

      gridToHome =
        deficit;

      digitalWrite(
        STATUS_LED,
        HIGH
      );
    }
  }

  // ===================================================
  // BALANCED
  // ===================================================
  else {

    mode =
      "IDLE";

    digitalWrite(
      STATUS_LED,
      HIGH
    );
  }
}

// =====================================================
// UPDATE BATTERY
// =====================================================
void updateBattery(
  float elapsedHours
) {

  // CHARGING
  if (
    mode ==
    "CHARGING"
  ) {

    float storedEnergy =
      solarToBattery
      *
      elapsedHours
      *
      CHARGE_EFFICIENCY
      *
      SIMULATION_SPEED;

    float percentage =
      storedEnergy
      /
      BATTERY_CAPACITY_WH
      *
      100.0f;

    batterySOC +=
      percentage;
  }

  // DISCHARGING
  if (
    mode ==
    "DISCHARGING"
  ) {

    float removedEnergy =
      batteryToHome
      *
      elapsedHours
      /
      DISCHARGE_EFFICIENCY
      *
      SIMULATION_SPEED;

    float percentage =
      removedEnergy
      /
      BATTERY_CAPACITY_WH
      *
      100.0f;

    batterySOC -=
      percentage;
  }

  batterySOC =
    limitFloat(
      batterySOC,
      MIN_SOC,
      MAX_SOC
    );
}

// =====================================================
// EFFICIENCY CALCULATIONS
// =====================================================
void calculateEfficiency() {

  // -----------------------------------------------
  // Solar self-consumption
  //
  // How much generated solar is actually used
  // by home or battery?
  // -----------------------------------------------

  if (
    solarPower >
    0.01f
  ) {

    solarSelfConsumption =
      (
        solarToHome +
        solarToBattery
      )
      /
      solarPower
      *
      100.0f;
  }

  else {

    solarSelfConsumption =
      0.0f;
  }

  // -----------------------------------------------
  // Solar coverage
  //
  // How much of home demand is directly supplied
  // by solar?
  // -----------------------------------------------

  if (
    homeLoad >
    0.01f
  ) {

    solarCoverage =
      solarToHome
      /
      homeLoad
      *
      100.0f;
  }

  else {

    solarCoverage =
      0.0f;
  }

  // -----------------------------------------------
  // Grid saving
  //
  // How much home consumption did not come
  // from grid?
  // -----------------------------------------------

  if (
    homeLoad >
    0.01f
  ) {

    gridSaving =
      (
        homeLoad -
        gridToHome
      )
      /
      homeLoad
      *
      100.0f;
  }

  else {

    gridSaving =
      0.0f;
  }

  solarSelfConsumption =
    limitFloat(
      solarSelfConsumption,
      0.0f,
      100.0f
    );

  solarCoverage =
    limitFloat(
      solarCoverage,
      0.0f,
      100.0f
    );

  gridSaving =
    limitFloat(
      gridSaving,
      0.0f,
      100.0f
    );
}

// =====================================================
// BUZZER
// =====================================================
void updateBuzzer() {

  static unsigned long lastToggle = 0;
  static bool buzzerState = false;

  // Only warning when:
  // battery low and grid becomes necessary
  if (
    mode ==
    "USE GRID"
  ) {

    if (
      millis() -
      lastToggle >=
      300
    ) {

      lastToggle =
        millis();

      buzzerState =
        !buzzerState;

      digitalWrite(
        BUZZER_PIN,
        buzzerState
      );
    }
  }

  else {

    digitalWrite(
      BUZZER_PIN,
      LOW
    );

    buzzerState =
      false;
  }
}

// =====================================================
// OLED
// =====================================================
void updateOLED() {

  display.clearDisplay();

  display.setTextSize(1);

  display.setTextColor(
    SSD1306_WHITE
  );

  display.setCursor(
    0,
    0
  );

  display.print("Solar:");
  display.print(
    solarPower,
    0
  );
  display.println("W");

  display.print("Load :");
  display.print(
    homeLoad,
    0
  );
  display.println("W");

  display.print("SOC  :");
  display.print(
    batterySOC,
    0
  );
  display.println("%");

  display.print("Grid :");
  display.print(
    gridToHome,
    0
  );
  display.println("W");

  display.print("Save :");
  display.print(
    gridSaving,
    0
  );
  display.println("%");

  display.print("Mode :");
  display.println(
    mode
  );

  display.display();
}

// =====================================================
// SERIAL MONITOR
// =====================================================
void printData() {

  Serial.println(
    "===================================="
  );

  Serial.print(
    "Sunlight: "
  );

  Serial.print(
    sunlightPercent,
    1
  );

  Serial.println("%");

  Serial.print(
    "Temperature: "
  );

  Serial.print(
    temperature,
    1
  );

  Serial.println(" C");

  Serial.print(
    "Humidity: "
  );

  Serial.print(
    humidity,
    1
  );

  Serial.println("%");

  Serial.print(
    "Solar Power: "
  );

  Serial.print(
    solarPower,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Home Load: "
  );

  Serial.print(
    homeLoad,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Battery SOC: "
  );

  Serial.print(
    batterySOC,
    1
  );

  Serial.println("%");

  Serial.println(
    "------------ ENERGY FLOW ------------"
  );

  Serial.print(
    "Solar -> Home: "
  );

  Serial.print(
    solarToHome,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Solar -> Battery: "
  );

  Serial.print(
    solarToBattery,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Battery -> Home: "
  );

  Serial.print(
    batteryToHome,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Grid -> Home: "
  );

  Serial.print(
    gridToHome,
    2
  );

  Serial.println(" W");

  Serial.print(
    "Unused Solar: "
  );

  Serial.print(
    excessSolar,
    2
  );

  Serial.println(" W");

  Serial.println(
    "------------ EFFICIENCY -------------"
  );

  Serial.print(
    "Solar Self Consumption: "
  );

  Serial.print(
    solarSelfConsumption,
    1
  );

  Serial.println("%");

  Serial.print(
    "Solar Home Coverage: "
  );

  Serial.print(
    solarCoverage,
    1
  );

  Serial.println("%");

  Serial.print(
    "Grid Saving: "
  );

  Serial.print(
    gridSaving,
    1
  );

  Serial.println("%");

  Serial.print(
    "MODE: "
  );

  Serial.println(
    mode
  );
}

// =====================================================
// SETUP
// =====================================================
void setup() {

  Serial.begin(115200);

  analogReadResolution(12);

  pinMode(
    CHARGE_LED,
    OUTPUT
  );

  pinMode(
    DISCHARGE_LED,
    OUTPUT
  );

  pinMode(
    STATUS_LED,
    OUTPUT
  );

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  resetOutputs();

  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  Wire.begin(
    21,
    22
  );

  if (
    !display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS
    )
  ) {

    Serial.println(
      "OLED ERROR"
    );

    while (true) {
      delay(100);
    }
  }

  dht.setup(
    DHT_PIN,
    DHTesp::DHT22
  );

  display.clearDisplay();

  display.setTextColor(
    SSD1306_WHITE
  );

  display.setTextSize(1);

  display.setCursor(
    0,
    0
  );

  display.println(
    "SMART HOME"
  );

  display.println(
    "SOLAR ENERGY"
  );

  display.println(
    "AUTOMATIC SYSTEM"
  );

  display.println();

  display.println(
    "Starting..."
  );

  display.display();

  previousTime =
    millis();

  delay(1500);
}

// =====================================================
// LOOP
// =====================================================
void loop() {

  unsigned long now =
    millis();

  float elapsedHours =
    (
      now -
      previousTime
    )
    /
    3600000.0f;

  previousTime =
    now;

  // 1. Read environment
  readSunlight();
  readWeather();

  // 2. Calculate solar generation
  calculateSolarPower();

  // 3. Calculate automatic home demand
  calculateHomeLoad();

  // 4. Solar/Battery/Grid decision
  manageEnergy();

  // 5. Battery charges/discharges automatically
  updateBattery(
    elapsedHours
  );

  // 6. Calculate system efficiencies
  calculateEfficiency();

  // 7. Warning system
  updateBuzzer();

  // 8. Display
  if (
    now -
    lastDisplayTime >=
    500
  ) {

    lastDisplayTime =
      now;

    updateOLED();
  }

  // 9. Serial report
  if (
    now -
    lastSerialTime >=
    1500
  ) {

    lastSerialTime =
      now;

    printData();
  }

  delay(10);
}
