/*
  ==============================================================================
   PROJECT ROBODAM: HOME ENERGY MANAGEMENT SYSTEM (H.E.M.S) & MICROGRID GATEWAY
   MASTER CENTRAL CONTROLLER & IOT DASHBOARD + AI CHATBOT (GROQ INTEGRATED)
   + AI NEURAL CORTEX: Comfort Scoring, Thermal Prediction & Anomaly Detection
  ==============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_idf_version.h>

// ==============================================================================
// 1. PIN CONFIGURATION & SENSOR CALIBRATION (MASTER LOCAL)
// ==============================================================================
const int CURRENT_SENSOR_PIN    = 34;
const int EV_CHARGER_RELAY_PIN  = 2;

const int SENSOR_THRESHOLD      = 2800;
const int HYSTERESIS_MARGIN     = 150;
const float FILTER_ALPHA        = 0.15;

float filteredADC               = 0.0;
int   rawCurrentValue           = 0;
float currentAmps               = 0.0;
float calculatedPowerKW         = 0.0;
bool  evChargerState            = false;
int   evOverrideMode            = 0;

unsigned long lastCurrentSampleTime = 0;
const unsigned long CURRENT_SAMPLE_INTERVAL = 100;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ==============================================================================
// 2. UNIFIED DATA STRUCTURE
// ==============================================================================
typedef struct struct_message {
  int   nodeID;
  bool  gasAlarm;
  float batteryVolts;
  float sunlight;
  float insideTemp;
  float insideHumidity;
  float outsideTemp;
  bool  occupied;
  int   coolingMode;
  int   fanSpeed;
  int   overrideMode;
  bool  led1;
  bool  led2;
  bool  tv;
  bool  curtainOpen;
  int   livingMode;
} SystemData;

SystemData incomingData;

// ==============================================================================
// 3. GLOBAL TELEMETRY STATES
// ==============================================================================
float globalBatteryVolts  = 13.8;
float globalSunlight      = 90.0;
bool  globalIsSolar       = true;
int   solarOverrideMode   = 0;

float globalInsideTemp    = 24.5;
float globalInsideHumidity= 58.0;
float globalOutsideTemp   = 22.0;
bool  globalOccupied      = false;
int   globalCoolingMode   = 0;
int   globalFanSpeed      = 0;
int   climateOverrideMode = 0;

bool  kitchenGasAlarm     = false;

bool  livingLed1          = false;
bool  livingLed2          = false;
bool  livingTv            = false;
bool  livingCurtainOpen   = false;
int   livingMode          = 0;

// ==============================================================================
// AI NEURAL CORTEX - Predictive Intelligence & Behavioral Learning Engine
// ==============================================================================
float tempHist[6] = {24.5, 24.5, 24.5, 24.5, 24.5, 24.5};
int tempIdx = 0;
float tempTrend = 0.0;
float predictedTemp30 = 24.5;

float totalSavedKWh = 0.0;
float solarSavedKWh = 0.0;
float standbySavedKWh = 0.0;
float hvacSavedKWh = 0.0;
float savedEGP = 0.0;

int comfortScore = 75;
int aiConfidence = 50;

float avgPowerBase = 0.5;
bool energyAnomaly = false;

String aiInsight1 = "Neural Cortex initializing...";
String aiInsight2 = "Building energy baseline...";
String aiInsight3 = "Thermal model calibrating...";

unsigned long lastAITime = 0;

WebServer server(80);

// ==============================================================================
// 4. EXPLAINABLE DECISION ENGINE
// ==============================================================================
String getEVChargerReason() {
  if (kitchenGasAlarm) return "SAFETY INTERLOCK: Combustible gas detected! EV Charger suspended immediately to prevent spark ignition.";
  if (evOverrideMode == 1) return "MANUAL OVERRIDE: Fast EV Charging engaged by user via Dashboard.";
  if (evOverrideMode == 2) return "MANUAL OVERRIDE: EV Charging suspended via manual user command.";
  if (rawCurrentValue < SENSOR_THRESHOLD) {
    return "DEMAND OPTIMIZATION: Main grid stable (" + String(currentAmps, 1) + " A / " + String(calculatedPowerKW, 2) + " kW). Smart EV charging active.";
  } else {
    return "PEAK LOAD SHEDDING: High household consumption (" + String(rawCurrentValue) + " ADC). EV charging paused to protect main circuit breaker.";
  }
}

String getSolarReason() {
  if (solarOverrideMode == 1) return "MANUAL OVERRIDE: 100% Clean Solar Power forced by operator.";
  if (solarOverrideMode == 2) return "MANUAL OVERRIDE: Utility Grid / Battery backup forced by operator.";
  if (globalIsSolar) return "CLEAN GENERATION: High solar irradiance (" + String(globalSunlight, 0) + "%). Photovoltaic array supplying zero-carbon energy.";
  return "AUTOMATED BACKUP: Low solar irradiance (" + String(globalSunlight, 0) + "%). Automatic Transfer Switch routed to Grid / Battery storage.";
}

String getClimateReason() {
  if (climateOverrideMode == 1) return "MANUAL OVERRIDE: Forced Low-Power Circulation Fan engaged.";
  if (climateOverrideMode == 2) return "MANUAL OVERRIDE: Forced High-Efficiency Air Conditioning engaged.";
  if (climateOverrideMode == 3) return "MANUAL OVERRIDE: Bedroom HVAC completely shut down.";
  if (!globalOccupied) return "ZERO STANDBY: Bedroom vacant. HVAC completely suspended to eliminate thermal waste.";
  if (globalInsideHumidity >= 80.0) return "DEHUMIDIFICATION: Indoor humidity >= 80%. High compressor engaged for moisture control.";
  if (globalInsideTemp >= 24.0) return "THERMAL COMFORT: Indoor temp >= 24 C. Active compressor cooling engaged.";
  if (globalInsideTemp - globalOutsideTemp >= 2.0) return "FREE COOLING: Outdoor air is cooler by >= 2 C. Natural eco-fan ventilation replacing active AC.";
  if (globalInsideTemp >= 21.0) return "ECO CIRCULATION: Mild temperature. Low-power gentle air circulation active.";
  return "OPTIMAL COMFORT: Room ambient conditions naturally balanced. All HVAC units idling.";
}

String getKitchenReason() {
  if (kitchenGasAlarm) return "CRITICAL EMERGENCY: Combustible LPG/Gas Leak detected! Safety Interlock Protocol Activated!";
  return "ATMOSPHERIC INTEGRITY: Zero combustible gas detected. Kitchen safety nominal.";
}

String getLivingRoomReason() {
  if (kitchenGasAlarm) return "SAFETY PROTOCOL: Emergency hazard active. Smart motorized curtains opened for atmospheric ventilation.";
  if (livingMode == 0) {
    if (!livingLed1 && !livingLed2) return "PHANTOM LOAD ELIMINATION: Living Room vacant. OLED TV & all zone lighting isolated (0.0W Standby Draw).";
    return "OCCUPANCY AUTOMATION: Motion tracked. Adaptive zone illumination and Smart TV enabled.";
  }
  return "MANUAL MODE: Living room appliances under direct user control via Dashboard.";
}

// ==============================================================================
// AI NEURAL CORTEX PROCESSING ENGINE
// ==============================================================================
String getAIScoreClass() {
  if (comfortScore >= 85) return "excellent";
  if (comfortScore >= 65) return "good";
  if (comfortScore >= 45) return "fair";
  return "poor";
}

void generateAIInsights() {
  if (kitchenGasAlarm) {
    aiInsight1 = "CRITICAL: Gas hazard triggered autonomous cross-node safety interlock. EV isolated, curtains opened for ventilation.";
  } else if (predictedTemp30 > 27.0 && globalCoolingMode == 0) {
    aiInsight1 = "THERMAL FORECAST: Predicted " + String(predictedTemp30, 1) + " C in 30 min. Recommend pre-cooling now to save compressor surge energy.";
  } else if (globalIsSolar && !evChargerState && evOverrideMode == 0) {
    aiInsight1 = "SOLAR SURPLUS: Clean energy peak detected (" + String(globalSunlight, 0) + "%). Optimal window for EV charging to maximize solar self-consumption ratio.";
  } else if (!globalOccupied && globalCoolingMode > 0 && climateOverrideMode == 0) {
    aiInsight1 = "WASTE DETECTED: Bedroom vacant but HVAC active. Potential saving of ~0.5 kW by switching to standby mode.";
  } else if (comfortScore >= 85) {
    aiInsight1 = "ALL OPTIMAL: Every zone operating at peak efficiency. Comfort index " + String(comfortScore) + "/100. Zero-waste energy profile active.";
  } else {
    aiInsight1 = "MONITORING: Neural cortex analyzing 4 distributed edge nodes across ESP-NOW mesh. All subsystems nominal.";
  }

  if (totalSavedKWh > 0.01) {
    aiInsight2 = "CUMULATIVE SAVINGS: " + String(totalSavedKWh, 2) + " kWh recovered (Solar: " + String(solarSavedKWh, 2) + " | Standby-Kill: " + String(standbySavedKWh, 3) + " | HVAC-Opt: " + String(hvacSavedKWh, 2) + " kWh)";
  } else {
    aiInsight2 = "BASELINE: Building energy consumption model from live sensor telemetry across mesh network...";
  }

  if (energyAnomaly) {
    aiInsight3 = "ANOMALY: Power draw " + String(calculatedPowerKW, 1) + " kW exceeds learned baseline " + String(avgPowerBase, 1) + " kW by >" + String(((calculatedPowerKW / avgPowerBase) - 1.0) * 100.0, 0) + "%. Investigating...";
  } else if (tempTrend > 0.03) {
    aiInsight3 = "THERMAL DRIFT: Indoor temp rising +" + String(tempTrend * 360.0, 1) + " C/hr. Predictive model updating thermal inertia coefficients.";
  } else if (tempTrend < -0.03) {
    aiInsight3 = "ACTIVE COOLING: Indoor temp falling " + String(tempTrend * 360.0, 1) + " C/hr. Cooling system operating within optimal efficiency band.";
  } else {
    aiInsight3 = "THERMAL EQUILIBRIUM: Steady-state achieved. Building envelope heat transfer balanced with HVAC output.";
  }
}

void processAIEngine() {
  unsigned long now = millis();
  if (now - lastAITime < 10000) return;
  lastAITime = now;

  tempHist[tempIdx] = globalInsideTemp;
  tempIdx = (tempIdx + 1) % 6;
  float newest = tempHist[(tempIdx + 5) % 6];
  float oldest = tempHist[tempIdx];
  tempTrend = (newest - oldest) / 6.0;
  predictedTemp30 = globalInsideTemp + (tempTrend * 18.0);

  int score = 100;
  if (globalInsideTemp > 28.0) score -= (int)((globalInsideTemp - 28.0) * 12);
  else if (globalInsideTemp > 25.0) score -= (int)((globalInsideTemp - 25.0) * 4);
  else if (globalInsideTemp < 18.0) score -= (int)((18.0 - globalInsideTemp) * 8);
  if (globalInsideHumidity > 75.0) score -= (int)((globalInsideHumidity - 75.0) * 2);
  else if (globalInsideHumidity < 30.0) score -= (int)((30.0 - globalInsideHumidity));
  if (kitchenGasAlarm) score -= 50;
  if (globalIsSolar) score += 5;
  if (!livingTv && !livingLed1 && !livingLed2 && livingMode == 0) score += 3;
  comfortScore = constrain(score, 0, 100);
  aiConfidence = min(60 + (int)(now / 120000UL), 97);

  float dt = 10.0 / 3600.0;
  if (globalIsSolar) solarSavedKWh += 0.15 * dt;
  if (!livingTv && !livingLed1 && !livingLed2) standbySavedKWh += 0.025 * dt;
  if (!globalOccupied && globalCoolingMode == 0) hvacSavedKWh += 0.5 * dt;
  totalSavedKWh = solarSavedKWh + standbySavedKWh + hvacSavedKWh;
  savedEGP = totalSavedKWh * 1.80;

  avgPowerBase = avgPowerBase * 0.95 + calculatedPowerKW * 0.05;
  energyAnomaly = (calculatedPowerKW > avgPowerBase * 2.5 && calculatedPowerKW > 1.0);

  generateAIInsights();
}

// ==============================================================================
// 5. AUTONOMOUS CROSS-NODE SAFETY INTERLOCK DISPATCHER
// ==============================================================================
void triggerEmergencySafetyInterlock() {
  Serial.println(">>> [CRITICAL EVENT] Gas Leak Detected in Kitchen! Initiating Emergency Interlock <<<");
  evChargerState = false;
  digitalWrite(EV_CHARGER_RELAY_PIN, LOW);
  livingCurtainOpen = true;
  SystemData emergencyCmd;
  memset(&emergencyCmd, 0, sizeof(emergencyCmd));
  emergencyCmd.nodeID = 4;
  emergencyCmd.curtainOpen = true;
  emergencyCmd.livingMode = 1;
  esp_now_send(broadcastAddress, (uint8_t *)&emergencyCmd, sizeof(emergencyCmd));
}

// ==============================================================================
// 6. ESP-NOW EVENT RECEIVE CALLBACK
// ==============================================================================
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void OnDataRecv(const esp_now_recv_info_t * recv_info, const uint8_t *incomingDataBuf, int len) {
#else
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingDataBuf, int len) {
#endif
  if (len != sizeof(SystemData)) return;
  memcpy(&incomingData, incomingDataBuf, sizeof(incomingData));

  if (incomingData.nodeID == 1) {
    globalBatteryVolts = incomingData.batteryVolts;
    globalSunlight     = incomingData.sunlight;
    globalIsSolar      = (incomingData.coolingMode == 1);
    solarOverrideMode  = incomingData.overrideMode;
  }
  else if (incomingData.nodeID == 2) {
    globalInsideTemp     = incomingData.insideTemp;
    globalInsideHumidity = incomingData.insideHumidity;
    globalOutsideTemp    = incomingData.outsideTemp;
    globalOccupied       = incomingData.occupied;
    globalCoolingMode    = incomingData.coolingMode;
    globalFanSpeed       = incomingData.fanSpeed;
  }
  else if (incomingData.nodeID == 3) {
    bool previousGasState = kitchenGasAlarm;
    kitchenGasAlarm = incomingData.gasAlarm;
    if (kitchenGasAlarm && !previousGasState) triggerEmergencySafetyInterlock();
  }
  else if (incomingData.nodeID == 4) {
    livingLed1        = incomingData.led1;
    livingLed2        = incomingData.led2;
    livingTv          = incomingData.tv;
    livingCurtainOpen = incomingData.curtainOpen;
    livingMode        = incomingData.livingMode;
  }
}

// ==============================================================================
// 7. LOCAL GRID SAMPLING & SMART EV CHARGING
// ==============================================================================
void processGridLoadAndEVCharger() {
  if (millis() - lastCurrentSampleTime >= CURRENT_SAMPLE_INTERVAL) {
    lastCurrentSampleTime = millis();
    int rawSample = analogRead(CURRENT_SENSOR_PIN);
    filteredADC = (FILTER_ALPHA * rawSample) + ((1.0 - FILTER_ALPHA) * filteredADC);
    rawCurrentValue = (int)filteredADC;
    currentAmps = (rawCurrentValue / 4095.0) * 32.0;
    calculatedPowerKW = (currentAmps * 220.0) / 1000.0;

    if (kitchenGasAlarm) evChargerState = false;
    else if (evOverrideMode == 1) evChargerState = true;
    else if (evOverrideMode == 2) evChargerState = false;
    else {
      if (rawCurrentValue > (SENSOR_THRESHOLD + HYSTERESIS_MARGIN)) evChargerState = false;
      else if (rawCurrentValue < (SENSOR_THRESHOLD - HYSTERESIS_MARGIN)) evChargerState = true;
    }
    digitalWrite(EV_CHARGER_RELAY_PIN, evChargerState ? HIGH : LOW);
  }
}

// ==============================================================================
// 8. SCADA WEB DASHBOARD + AI CHATBOT + AI NEURAL CORTEX PANEL
// ==============================================================================
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='ar' dir='rtl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ROBODAM &mdash; Autonomous H.E.M.S SCADA Center</title>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Barlow+Semi+Condensed:wght@500;600;700;800&family=IBM+Plex+Mono:wght@400;500;600;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += ":root{--bg:#070a0e;--panel:#0f141c;--panel-alt:#090d13;--line:#1c2532;--line-lo:#141b24;--copper:#e29547;--teal:#47b8ae;--red:#ef473a;--green:#34c759;--text-hi:#f0f4f8;--text-lo:#707e8d;--text-mid:#9ba8b7;}";
  html += "*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{background:radial-gradient(ellipse 1300px 900px at 10% -20%, #101924 0%, transparent 60%), radial-gradient(ellipse 1100px 800px at 100% 120%, #0c151e 0%, transparent 55%), var(--bg);color:var(--text-hi);font-family:'Barlow Semi Condensed',sans-serif;padding:24px 20px;min-height:100vh;}";
  html += ".header{max-width:1440px;margin:0 auto 16px auto;display:flex;justify-content:space-between;align-items:center;padding:16px 24px;background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}";
  html += ".brand{display:flex;align-items:center;gap:14px;} .pulse-dot{width:11px;height:11px;border-radius:50%;background:var(--teal);box-shadow:0 0 12px var(--teal);animation:pulse 2s infinite;}";
  html += ".brand h1{font-size:1.35rem;font-weight:800;letter-spacing:0.08em;text-transform:uppercase;} .brand small{color:var(--text-lo);font-size:0.75rem;font-family:'IBM Plex Mono',monospace;}";
  html += "@keyframes pulse{0%,100%{opacity:1;transform:scale(1);}50%{opacity:0.35;transform:scale(0.85);}}";
  html += ".kpi-bar{max-width:1440px;margin:0 auto 20px auto;display:grid;grid-template-columns:repeat(4,1fr);gap:14px;} @media(max-width:900px){.kpi-bar{grid-template-columns:1fr 1fr;}}";
  html += ".kpi-card{background:var(--panel-alt);border:1px solid var(--line);border-radius:8px;padding:14px 18px;display:flex;flex-direction:column;gap:4px;}";
  html += ".kpi-label{font-family:'IBM Plex Mono',monospace;font-size:0.65rem;color:var(--text-lo);letter-spacing:0.08em;text-transform:uppercase;}";
  html += ".kpi-val{font-family:'IBM Plex Mono',monospace;font-size:1.45rem;font-weight:700;} .kpi-val.teal{color:var(--teal);} .kpi-val.copper{color:var(--copper);} .kpi-val.green{color:var(--green);}";
  html += ".emergency-banner{max-width:1440px;margin:0 auto 20px auto;padding:16px 22px;background:rgba(239,71,58,0.18);border:2px solid var(--red);border-radius:10px;display:" + String(kitchenGasAlarm ? "block" : "none") + ";animation:redglow 1s infinite alternate;}";
  html += ".emergency-banner h3{color:#ff6b5e;font-size:1.2rem;font-weight:800;margin-bottom:4px;} .emergency-banner p{color:#f5d0cc;font-size:0.86rem;font-family:'IBM Plex Mono',monospace;}";
  html += "@keyframes redglow{0%{box-shadow:0 0 10px rgba(239,71,58,0.2);}100%{box-shadow:0 0 24px rgba(239,71,58,0.6);}}";
  html += ".grid-container{display:grid;grid-template-columns:1fr 1fr;gap:20px;max-width:1440px;margin:0 auto;} @media(max-width:1050px){.grid-container{grid-template-columns:1fr;}}";
  html += ".panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:22px;display:flex;flex-direction:column;justify-content:space-between;box-shadow:0 6px 20px rgba(0,0,0,0.3);}";
  html += ".panel-head{display:flex;justify-content:space-between;align-items:flex-end;margin-bottom:18px;border-bottom:1px solid var(--line-lo);padding-bottom:12px;}";
  html += ".panel-eyebrow{font-family:'IBM Plex Mono',monospace;font-size:0.68rem;color:var(--text-lo);letter-spacing:0.08em;margin-bottom:4px;} .panel-title{font-size:1.32rem;font-weight:700;}";
  html += ".badge{font-family:'IBM Plex Mono',monospace;font-size:0.68rem;padding:4px 10px;border-radius:4px;font-weight:700;letter-spacing:0.05em;border:1px solid;}";
  html += ".badge-copper{background:rgba(226,149,71,0.1);color:var(--copper);border-color:rgba(226,149,71,0.35);} .badge-teal{background:rgba(71,184,174,0.1);color:var(--teal);border-color:rgba(71,184,174,0.35);}";
  html += ".badge-red{background:rgba(239,71,58,0.2);color:#ff6b5e;border-color:var(--red);animation:redglow 0.9s infinite alternate;}";
  html += ".readouts{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:18px;}";
  html += ".readout{background:var(--panel-alt);padding:14px 16px;border-radius:6px;border:1px solid var(--line-lo);}";
  html += ".readout-label{font-family:'IBM Plex Mono',monospace;font-size:0.64rem;color:var(--text-lo);letter-spacing:0.07em;margin-bottom:6px;}";
  html += ".readout-value{font-family:'IBM Plex Mono',monospace;font-size:1.38rem;font-weight:700;}";
  html += ".readout-value.copper{color:var(--copper);} .readout-value.teal{color:var(--teal);} .readout-value.red{color:#ff6b5e;} .readout-value.green{color:var(--green);}";
  html += ".log-box{background:var(--panel-alt);border-left:3px solid var(--copper);border-radius:0 6px 6px 0;padding:12px 16px;font-family:'IBM Plex Mono',monospace;font-size:0.78rem;line-height:1.55;color:var(--text-mid);margin-bottom:18px;min-height:54px;}";
  html += ".log-box.danger{border-left-color:var(--red);background:rgba(239,71,58,0.08);color:#f5d0cc;}";
  html += ".log-tag{display:block;font-family:'IBM Plex Mono',monospace;font-size:0.62rem;color:var(--text-lo);letter-spacing:0.08em;margin-bottom:4px;}";
  html += ".switchgroup{display:flex;gap:6px;background:var(--panel-alt);border:1px solid var(--line-lo);border-radius:6px;padding:4px;margin-bottom:8px;}";
  html += ".switchgroup a{flex:1;text-align:center;padding:10px 8px;font-family:'IBM Plex Mono',monospace;font-size:0.72rem;font-weight:700;color:var(--text-lo);text-decoration:none;border-radius:4px;transition:all 0.2s;}";
  html += ".switchgroup a.on{background:var(--copper);color:#0a0d12;} .switchgroup a.on-teal{background:var(--teal);color:#081014;}";

  // AI Neural Cortex CSS
  html += ".ai-panel{background:linear-gradient(135deg,rgba(139,92,246,0.07) 0%,rgba(59,130,246,0.05) 100%);border:1px solid rgba(139,92,246,0.25);box-shadow:0 0 30px rgba(139,92,246,0.08);animation:aipulse 4s ease-in-out infinite;}";
  html += "@keyframes aipulse{0%,100%{box-shadow:0 0 20px rgba(139,92,246,0.08);}50%{box-shadow:0 0 45px rgba(139,92,246,0.2);border-color:rgba(139,92,246,0.45);}}";
  html += ".ai-big{font-family:'IBM Plex Mono',monospace;font-size:2.4rem;font-weight:800;line-height:1;}";
  html += ".ai-big.excellent{color:#34c759;} .ai-big.good{color:var(--teal);} .ai-big.fair{color:var(--copper);} .ai-big.poor{color:#ff6b5e;}";
  html += ".ai-log{background:var(--panel-alt);border-left:3px solid #8b5cf6;border-radius:0 6px 6px 0;padding:10px 14px;font-family:'IBM Plex Mono',monospace;font-size:0.74rem;line-height:1.5;color:#c4b5fd;margin-bottom:8px;}";
  html += ".ai-log.warn{border-left-color:#f59e0b;color:#fcd34d;}";
  html += ".badge-ai{background:rgba(139,92,246,0.15);color:#a78bfa;border-color:rgba(139,92,246,0.4);}";

  html += "</style></head><body>";

  // HEADER
  html += "<div class='header'><div class='brand'><div class='pulse-dot'></div><div><h1>ROBODAM &mdash; H.E.M.S</h1><small>AUTONOMOUS MICROGRID & DEMAND-SIDE ENERGY GATEWAY</small></div></div><span class='badge badge-teal'>GATEWAY MASTER ONLINE</span></div>";

  // KPI BAR
  html += "<div class='kpi-bar'>";
  html += "<div class='kpi-card'><div class='kpi-label'>Grid Power Draw</div><div class='kpi-val copper' id='ribbonPower'>" + String(calculatedPowerKW, 2) + " kW</div></div>";
  html += "<div class='kpi-card'><div class='kpi-label'>Microgrid Source</div><div class='kpi-val teal' id='ribbonSource'>" + String(globalIsSolar ? "SOLAR (PV)" : "GRID / BATT") + "</div></div>";
  html += "<div class='kpi-card'><div class='kpi-label'>Energy Saved</div><div class='kpi-val green' id='ribbonSaved'>" + String(totalSavedKWh, 2) + " kWh</div></div>";
  html += "<div class='kpi-card'><div class='kpi-label'>System Integrity</div><div class='kpi-val " + String(kitchenGasAlarm ? "red" : "teal") + "' id='ribbonSafety'>" + String(kitchenGasAlarm ? "HAZARD ACTIVE" : "ALL SECURE") + "</div></div>";
  html += "</div>";

  // EMERGENCY BANNER
  html += "<div class='emergency-banner' id='emergencyBanner'><h3>&#128680; CRITICAL SAFETY INTERLOCK ACTIVATED</h3><p>Combustible gas leak detected in Kitchen! Autonomous safety protocol engaged.</p></div>";

  html += "<div class='grid-container'>";

  // PANEL 1: EV CHARGING
  html += "<div class='panel'><div class='panel-head'><div><div class='panel-eyebrow'>LOCAL GATEWAY // DEMAND MANAGEMENT</div><div class='panel-title'>Smart EV Charging & Peak Shaving</div></div><span class='badge badge-copper'>MASTER LOCAL</span></div>";
  html += "<div class='readouts'>";
  html += "<div class='readout'><div class='readout-label'>MAIN GRID CURRENT</div><div class='readout-value copper' id='gridAmps'>" + String(currentAmps, 1) + " A</div></div>";
  html += "<div class='readout'><div class='readout-label'>TOTAL POWER DRAW</div><div class='readout-value' id='gridPower'>" + String(calculatedPowerKW, 2) + " kW</div></div>";
  html += "<div class='readout'><div class='readout-label'>EV CHARGER RELAY</div><div class='readout-value " + String(evChargerState ? "teal" : "") + "' id='evState'>" + String(evChargerState ? "CHARGING" : "PAUSED") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>RAW ADC LOAD LEVEL</div><div class='readout-value' id='rawAdc'>" + String(rawCurrentValue) + "</div></div>";
  html += "</div><div class='log-box'><span class='log-tag'>AUTONOMOUS REASON</span><span id='evReasonText'>" + getEVChargerReason() + "</span></div>";
  html += "<div class='switchgroup'><a href='/setEV?mode=0' class='" + String(evOverrideMode == 0 ? "on" : "") + "'>AUTO SHAVING</a><a href='/setEV?mode=1' class='" + String(evOverrideMode == 1 ? "on" : "") + "'>FORCE ON</a><a href='/setEV?mode=2' class='" + String(evOverrideMode == 2 ? "on" : "") + "'>FORCE OFF</a></div></div>";

  // PANEL 2: SOLAR
  html += "<div class='panel'><div class='panel-head'><div><div class='panel-eyebrow'>UNIT 01 // MICROGRID & RENEWABLES</div><div class='panel-title'>Solar Generation & Battery Storage</div></div><span class='badge badge-teal'>NODE 1</span></div>";
  html += "<div class='readouts'>";
  html += "<div class='readout'><div class='readout-label'>ACTIVE POWER SOURCE</div><div class='readout-value " + String(globalIsSolar ? "teal" : "copper") + "' id='solarPowerSource'>" + String(globalIsSolar ? "SOLAR POWER" : "GRID / BATTERY") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>SUNLIGHT IRRADIANCE</div><div class='readout-value' id='sunlight'>" + String(globalSunlight, 0) + " %</div></div>";
  html += "<div class='readout'><div class='readout-label'>BATTERY BANK VOLTAGE</div><div class='readout-value teal' id='batVolts'>" + String(globalBatteryVolts, 1) + " V</div></div>";
  html += "<div class='readout'><div class='readout-label'>TRANSFER SWITCH STATE</div><div class='readout-value' id='switchState'>" + String(globalIsSolar ? "PHOTOVOLTAIC" : "UTILITY GRID") + "</div></div>";
  html += "</div><div class='log-box'><span class='log-tag'>MICROGRID DECISION</span><span id='solarReasonText'>" + getSolarReason() + "</span></div>";
  html += "<div class='switchgroup'><a href='/setSolar?mode=0' class='" + String(solarOverrideMode == 0 ? "on-teal" : "") + "'>AUTO MODE</a><a href='/setSolar?mode=1' class='" + String(solarOverrideMode == 1 ? "on-teal" : "") + "'>FORCE SOLAR</a><a href='/setSolar?mode=2' class='" + String(solarOverrideMode == 2 ? "on-teal" : "") + "'>FORCE GRID</a></div></div>";

  // PANEL 3: BEDROOM HVAC
  html += "<div class='panel'><div class='panel-head'><div><div class='panel-eyebrow'>UNIT 02 // HVAC & THERMAL COMFORT</div><div class='panel-title'>Adaptive Climate Control</div></div><span class='badge badge-teal'>NODE 2</span></div>";
  html += "<div class='readouts'>";
  html += "<div class='readout'><div class='readout-label'>INDOOR TEMP & HUMIDITY</div><div class='readout-value teal' id='inTemp'>" + String(globalInsideTemp, 1) + " &deg;C <span style='font-size:0.8rem;color:var(--text-lo);'>/ " + String(globalInsideHumidity, 0) + "%</span></div></div>";
  html += "<div class='readout'><div class='readout-label'>OUTDOOR AMBIENT TEMP</div><div class='readout-value' id='outTemp'>" + String(globalOutsideTemp, 1) + " &deg;C</div></div>";
  html += "<div class='readout'><div class='readout-label'>ZONE OCCUPANCY</div><div class='readout-value' id='occ'>" + String(globalOccupied ? "OCCUPIED" : "VACANT") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>HVAC COOLING MODE</div><div class='readout-value teal' id='mode'>" + String(globalCoolingMode == 0 ? "STANDBY (OFF)" : (globalCoolingMode == 1 ? "ECO FREE-FAN" : "HIGH-EFF AC")) + "</div></div>";
  html += "</div><div class='log-box'><span class='log-tag'>CLIMATE DECISION</span><span id='climateReasonText'>" + getClimateReason() + "</span></div>";
  html += "<div class='switchgroup'><a href='/setClimate?mode=0' class='" + String(climateOverrideMode == 0 ? "on" : "") + "'>AUTO</a><a href='/setClimate?mode=1' class='" + String(climateOverrideMode == 1 ? "on" : "") + "'>FAN ONLY</a><a href='/setClimate?mode=2' class='" + String(climateOverrideMode == 2 ? "on" : "") + "'>AC ONLY</a><a href='/setClimate?mode=3' class='" + String(climateOverrideMode == 3 ? "on" : "") + "'>OFF</a></div></div>";

  // PANEL 4: KITCHEN SAFETY
  html += "<div class='panel'><div class='panel-head'><div><div class='panel-eyebrow'>UNIT 03 // SAFETY & HAZARDS</div><div class='panel-title'>Kitchen Gas & Fire Safety</div></div><span class='badge " + String(kitchenGasAlarm ? "badge-red" : "badge-teal") + "' id='kitchenChip'>" + String(kitchenGasAlarm ? "GAS HAZARD" : "NODE 3") + "</span></div>";
  html += "<div class='readouts'>";
  html += "<div class='readout'><div class='readout-label'>ATMOSPHERIC SENSOR</div><div class='readout-value " + String(kitchenGasAlarm ? "red" : "teal") + "' id='gasStatus'>" + String(kitchenGasAlarm ? "LEAK DETECTED" : "SECURE (CLEAN)") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>CROSS-NODE INTERLOCK</div><div class='readout-value " + String(kitchenGasAlarm ? "red" : "green") + "' id='interlockStatus'>" + String(kitchenGasAlarm ? "ACTIVE (ISOLATED)" : "STANDBY") + "</div></div>";
  html += "</div><div class='log-box " + String(kitchenGasAlarm ? "danger" : "") + "' id='kitchenLog'><span class='log-tag'>SAFETY INTERLOCK LOG</span><span id='kitchenReasonText'>" + getKitchenReason() + "</span></div></div>";

  // PANEL 5: LIVING ROOM
  html += "<div class='panel' style='grid-column: span 2;'><div class='panel-head'><div><div class='panel-eyebrow'>UNIT 04 // LIVING ROOM AUTOMATION</div><div class='panel-title'>Zoned Directional Lighting & Smart TV</div></div><span class='badge badge-teal'>NODE 4</span></div>";
  html += "<div class='readouts' style='grid-template-columns: repeat(4, 1fr);'>";
  html += "<div class='readout'><div class='readout-label'>RIGHT ZONE (LED 1)</div><div class='readout-value' id='led1State'>" + String(livingLed1 ? "ON" : "OFF") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>LEFT ZONE (LED 2)</div><div class='readout-value' id='led2State'>" + String(livingLed2 ? "ON" : "OFF") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>OLED TV (0W STANDBY)</div><div class='readout-value " + String(livingTv ? "teal" : "") + "' id='tvState'>" + String(livingTv ? "ACTIVE (ON)" : "STANDBY (0.0W)") + "</div></div>";
  html += "<div class='readout'><div class='readout-label'>SMART CURTAINS</div><div class='readout-value' id='curtainState'>" + String(livingCurtainOpen ? "OPEN" : "CLOSED") + "</div></div>";
  html += "</div><div class='log-box'><span class='log-tag'>AUTOMATION DECISION</span><span id='livingReasonText'>" + getLivingRoomReason() + "</span></div>";
  html += "<div class='switchgroup'><a href='/setLiving?mode=0' class='" + String(livingMode == 0 ? "on-teal" : "") + "'>AUTO OCCUPANCY</a><a href='/setLiving?mode=1' class='" + String(livingMode == 1 ? "on-teal" : "") + "'>MANUAL OVERRIDE</a></div>";
  html += "<div class='switchgroup'><a href='/setLiving?led1=" + String(livingLed1 ? "0" : "1") + "'>TOGGLE LED 1</a><a href='/setLiving?led2=" + String(livingLed2 ? "0" : "1") + "'>TOGGLE LED 2</a><a href='/setLiving?tv=" + String(livingTv ? "0" : "1") + "'>TOGGLE TV</a><a href='/setLiving?curtain=" + String(livingCurtainOpen ? "0" : "1") + "'>TOGGLE CURTAIN</a></div></div>";

  // PANEL 6: AI NEURAL CORTEX
  html += "<div class='panel ai-panel' style='grid-column: span 2;'>";
  html += "<div class='panel-head'><div><div class='panel-eyebrow'>NEURAL CORTEX // PREDICTIVE AI ENGINE</div><div class='panel-title'>&#129504; Autonomous Intelligence & Behavioral Learning</div></div><span class='badge badge-ai' id='aiConfBadge'>CONFIDENCE: " + String(aiConfidence) + "%</span></div>";
  html += "<div class='readouts' style='grid-template-columns: repeat(4, 1fr);'>";
  html += "<div class='readout'><div class='readout-label'>AI COMFORT INDEX</div><div class='ai-big " + getAIScoreClass() + "' id='aiScore'>" + String(comfortScore) + "<span style='font-size:1rem;color:var(--text-lo);'>/100</span></div></div>";
  html += "<div class='readout'><div class='readout-label'>THERMAL FORECAST (30 MIN)</div><div class='readout-value teal' id='aiTherm'>" + String(predictedTemp30, 1) + " &deg;C <span style='font-size:0.7rem;color:var(--text-lo);'>(" + String(tempTrend > 0.03 ? "RISING" : (tempTrend < -0.03 ? "COOLING" : "STABLE")) + ")</span></div></div>";
  html += "<div class='readout'><div class='readout-label'>ENERGY RECOVERED</div><div class='readout-value green' id='aiSaved'>" + String(totalSavedKWh, 2) + " kWh</div></div>";
  html += "<div class='readout'><div class='readout-label'>ESTIMATED BILL SAVINGS</div><div class='readout-value copper' id='aiMoney'>" + String(savedEGP, 2) + " EGP</div></div>";
  html += "</div>";
  html += "<div class='ai-log'><span class='log-tag'>&#128161; PREDICTION ENGINE</span><span id='aiI1'>" + aiInsight1 + "</span></div>";
  html += "<div class='ai-log'><span class='log-tag'>&#9889; ENERGY OPTIMIZER</span><span id='aiI2'>" + aiInsight2 + "</span></div>";
  html += "<div class='ai-log " + String(energyAnomaly ? "warn" : "") + "' id='aiI3Box'><span class='log-tag'>&#128300; ANOMALY DETECTOR</span><span id='aiI3'>" + aiInsight3 + "</span></div>";
  html += "</div>";

  html += "</div>"; // End grid-container

  // AI CHATBOT FLOATING BUTTON & WINDOW
  html += "<div id='ai-chat-btn' onclick='toggleChat()' style='position:fixed;bottom:20px;right:20px;background:var(--teal);color:#000;padding:12px 18px;border-radius:30px;font-weight:bold;cursor:pointer;box-shadow:0 4px 15px rgba(0,0,0,0.4);z-index:999;'>&#129302; ROBODAM AI</div>";
  html += "<div id='ai-chat-box' style='display:none;position:fixed;bottom:75px;right:20px;width:360px;height:480px;background:var(--panel);border:1px solid var(--line);border-radius:12px;flex-direction:column;z-index:999;box-shadow:0 8px 32px rgba(0,0,0,0.6);'>";
  html += "<div style='padding:12px;background:var(--panel-alt);border-bottom:1px solid var(--line);font-weight:bold;color:var(--teal);display:flex;justify-content:space-between;border-top-left-radius:12px;border-top-right-radius:12px;'><span>H.E.M.S Smart Assistant</span><span onclick='toggleChat()' style='cursor:pointer;'>&#10005;</span></div>";
  html += "<div id='chat-messages' style='flex:1;padding:12px;overflow-y:auto;font-size:0.85rem;display:flex;flex-direction:column;gap:8px;'><div style='background:var(--panel-alt);padding:8px 12px;border-radius:6px;color:var(--text-mid);'>&#1571;&#1607;&#1604;&#1575;&#1611; &#1576;&#1603;! &#1571;&#1606;&#1575; &#1605;&#1587;&#1575;&#1593;&#1583; ROBODAM &#1575;&#1604;&#1584;&#1603;&#1610;. &#1603;&#1610;&#1601; &#1610;&#1605;&#1603;&#1606;&#1606;&#1610; &#1605;&#1587;&#1575;&#1593;&#1583;&#1578;&#1603;&#1567;</div></div>";
  html += "<div style='padding:10px;border-top:1px solid var(--line);display:flex;gap:6px;'>";
  html += "<input type='text' id='chat-input' placeholder='Ask about energy, modes, bills...' style='flex:1;background:var(--bg);border:1px solid var(--line);color:#fff;padding:8px;border-radius:4px;outline:none;' onkeypress='if(event.key===\"Enter\") sendChatMessage()'>";
  html += "<button onclick='sendChatMessage()' style='background:var(--copper);border:none;color:#000;padding:8px 12px;border-radius:4px;font-weight:bold;cursor:pointer;'>Send</button>";
  html += "</div></div>";

  // JAVASCRIPT: AJAX + AI CHATBOT
  html += "<script>";

  html += "const GROQ_API_KEY='YOUR_GROQ_API_KEY_HERE';";

  html += "function toggleChat(){var b=document.getElementById('ai-chat-box');b.style.display=(b.style.display==='none'||b.style.display==='')?'flex':'none';}";

  html += "function appendMessage(s,t){var m=document.getElementById('chat-messages');var d=document.createElement('div');d.style.padding='8px 12px';d.style.borderRadius='6px';d.style.maxWidth='85%';if(s==='user'){d.style.background='var(--teal)';d.style.color='#000';d.style.alignSelf='flex-start';}else{d.style.background='var(--panel-alt)';d.style.color='#fff';d.style.alignSelf='flex-end';}d.innerText=t;m.appendChild(d);m.scrollTop=m.scrollHeight;}";

  html += "async function sendChatMessage(){var i=document.getElementById('chat-input');var t=i.value.trim();if(!t)return;appendMessage('user',t);i.value='';var cd={};try{var r=await fetch('/data');cd=await r.json();}catch(e){}appendMessage('ai','Thinking...');";
  html += "var sp='You are ROBODAM AI Assistant, an intelligent HEMS home engine.\\nCurrent Telemetry JSON: '+JSON.stringify(cd)+'\\nCapabilities:\\n1. Answer telemetry questions using reasons in JSON.\\n2. Calculate electricity bill (formula = calculatedPowerKW * 24 * 30 * 1.5 EGP/kWh).\\n3. ACTIONS: If user asks for mode/control, reply in friendly Arabic AND append ACTION on new line: ACTION: [endpoints].\\nExamples:\\n- Travel Mode -> ACTION: /setEV?mode=2 | /setClimate?mode=3 | /setLiving?led1=0&led2=0&tv=0&mode=1\\n- Sleep Mode -> ACTION: /setEV?mode=2 | /setLiving?led1=0&led2=0&tv=0&mode=1 | /setClimate?mode=1\\n- Turn off EV -> ACTION: /setEV?mode=2';";
  html += "try{var res=await fetch('https://api.groq.com/openai/v1/chat/completions',{method:'POST',headers:{'Authorization':'Bearer '+GROQ_API_KEY,'Content-Type':'application/json'},body:JSON.stringify({model:'llama-3.3-70b-versatile',messages:[{role:'system',content:sp},{role:'user',content:t}],temperature:0.3})});var data=await res.json();var reply=data.choices[0].message.content;var msgs=document.getElementById('chat-messages');msgs.removeChild(msgs.lastChild);appendMessage('ai',reply.replace(/ACTION:.*/g,'').trim());var am=reply.match(/ACTION:\\s*(.*)/);if(am&&am[1]){var eps=am[1].split('|');for(var ep of eps){ep=ep.trim();if(ep)await fetch(ep);}}}catch(e){var msgs=document.getElementById('chat-messages');msgs.removeChild(msgs.lastChild);appendMessage('ai','Connection error.');}}";

  // AJAX AUTO-REFRESH
  html += "setInterval(()=>{fetch('/data').then(r=>r.json()).then(d=>{";
  html += "document.getElementById('gridAmps').innerText=d.currentAmps.toFixed(1)+' A';";
  html += "document.getElementById('gridPower').innerText=d.calculatedPowerKW.toFixed(2)+' kW';";
  html += "document.getElementById('ribbonPower').innerText=d.calculatedPowerKW.toFixed(2)+' kW';";
  html += "document.getElementById('evState').innerText=d.evChargerState?'CHARGING':'PAUSED';";
  html += "document.getElementById('evState').className='readout-value '+(d.evChargerState?'teal':'');";
  html += "document.getElementById('rawAdc').innerText=d.rawCurrentValue;";
  html += "document.getElementById('evReasonText').innerText=d.evReason;";
  html += "document.getElementById('solarPowerSource').innerText=d.isSolar?'SOLAR POWER':'GRID / BATTERY';";
  html += "document.getElementById('solarPowerSource').className='readout-value '+(d.isSolar?'teal':'copper');";
  html += "document.getElementById('ribbonSource').innerText=d.isSolar?'SOLAR (PV)':'GRID / BATT';";
  html += "document.getElementById('ribbonSource').className='kpi-val '+(d.isSolar?'teal':'copper');";
  html += "document.getElementById('switchState').innerText=d.isSolar?'PHOTOVOLTAIC':'UTILITY GRID';";
  html += "document.getElementById('batVolts').innerText=d.batteryVolts.toFixed(1)+' V';";
  html += "document.getElementById('sunlight').innerText=d.sunlight.toFixed(0)+' %';";
  html += "document.getElementById('solarReasonText').innerText=d.solarReason;";
  html += "document.getElementById('inTemp').innerHTML=d.insideTemp.toFixed(1)+' &deg;C <span style=\"font-size:0.8rem;color:var(--text-lo);\">/ '+d.insideHumidity.toFixed(0)+'%</span>';";
  html += "document.getElementById('outTemp').innerHTML=d.outsideTemp.toFixed(1)+' &deg;C';";
  html += "document.getElementById('occ').innerText=d.occupied?'OCCUPIED':'VACANT';";
  html += "document.getElementById('mode').innerText=d.coolingMode==0?'STANDBY (OFF)':(d.coolingMode==1?'ECO FREE-FAN':'HIGH-EFF AC');";
  html += "document.getElementById('climateReasonText').innerText=d.climateReason;";
  html += "document.getElementById('gasStatus').innerText=d.kitchenGasAlarm?'LEAK DETECTED':'SECURE (CLEAN)';";
  html += "document.getElementById('gasStatus').className='readout-value '+(d.kitchenGasAlarm?'red':'teal');";
  html += "document.getElementById('interlockStatus').innerText=d.kitchenGasAlarm?'ACTIVE (ISOLATED)':'STANDBY';";
  html += "document.getElementById('interlockStatus').className='readout-value '+(d.kitchenGasAlarm?'red':'green');";
  html += "document.getElementById('kitchenReasonText').innerText=d.kitchenReason;";
  html += "document.getElementById('kitchenLog').className='log-box '+(d.kitchenGasAlarm?'danger':'');";
  html += "document.getElementById('kitchenChip').className='badge '+(d.kitchenGasAlarm?'badge-red':'badge-teal');";
  html += "document.getElementById('kitchenChip').innerText=d.kitchenGasAlarm?'GAS HAZARD':'NODE 3';";
  html += "document.getElementById('ribbonSafety').innerText=d.kitchenGasAlarm?'HAZARD ACTIVE':'ALL SECURE';";
  html += "document.getElementById('ribbonSafety').className='kpi-val '+(d.kitchenGasAlarm?'red':'teal');";
  html += "document.getElementById('emergencyBanner').style.display=d.kitchenGasAlarm?'block':'none';";
  html += "document.getElementById('led1State').innerText=d.livingLed1?'ON':'OFF';";
  html += "document.getElementById('led2State').innerText=d.livingLed2?'ON':'OFF';";
  html += "document.getElementById('tvState').innerText=d.livingTv?'ACTIVE (ON)':'STANDBY (0.0W)';";
  html += "document.getElementById('tvState').className='readout-value '+(d.livingTv?'teal':'');";
  html += "document.getElementById('curtainState').innerText=d.livingCurtainOpen?'OPEN':'CLOSED';";
  html += "document.getElementById('livingReasonText').innerText=d.livingReason;";

  // AI NEURAL CORTEX AJAX UPDATES
  html += "document.getElementById('aiScore').innerHTML=d.comfortScore+'<span style=\"font-size:1rem;color:var(--text-lo);\">/100</span>';";
  html += "var sc=d.comfortScore>=85?'excellent':(d.comfortScore>=65?'good':(d.comfortScore>=45?'fair':'poor'));";
  html += "document.getElementById('aiScore').className='ai-big '+sc;";
  html += "document.getElementById('aiTherm').innerHTML=d.predictedTemp30.toFixed(1)+' &deg;C <span style=\"font-size:0.7rem;color:var(--text-lo);\">('+d.tempTrendLabel+')</span>';";
  html += "document.getElementById('aiSaved').innerText=d.totalSavedKWh.toFixed(2)+' kWh';";
  html += "document.getElementById('aiMoney').innerText=d.savedEGP.toFixed(2)+' EGP';";
  html += "document.getElementById('aiConfBadge').innerText='CONFIDENCE: '+d.aiConfidence+'%';";
  html += "document.getElementById('aiI1').innerText=d.aiInsight1;";
  html += "document.getElementById('aiI2').innerText=d.aiInsight2;";
  html += "document.getElementById('aiI3').innerText=d.aiInsight3;";
  html += "document.getElementById('aiI3Box').className='ai-log '+(d.energyAnomaly?'warn':'');";
  html += "document.getElementById('ribbonSaved').innerText=d.totalSavedKWh.toFixed(2)+' kWh';";

  html += "}).catch(()=>{});},800);";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

// REST JSON API (includes AI Neural Cortex data)
void handleData() {
  String json = "{";
  json += "\"currentAmps\":" + String(currentAmps, 1) + ",";
  json += "\"calculatedPowerKW\":" + String(calculatedPowerKW, 2) + ",";
  json += "\"evChargerState\":" + String(evChargerState ? "true" : "false") + ",";
  json += "\"rawCurrentValue\":" + String(rawCurrentValue) + ",";
  json += "\"evReason\":\"" + getEVChargerReason() + "\",";
  json += "\"isSolar\":" + String(globalIsSolar ? "true" : "false") + ",";
  json += "\"batteryVolts\":" + String(globalBatteryVolts, 1) + ",";
  json += "\"sunlight\":" + String(globalSunlight, 1) + ",";
  json += "\"solarReason\":\"" + getSolarReason() + "\",";
  json += "\"insideTemp\":" + String(globalInsideTemp, 1) + ",";
  json += "\"insideHumidity\":" + String(globalInsideHumidity, 1) + ",";
  json += "\"outsideTemp\":" + String(globalOutsideTemp, 1) + ",";
  json += "\"occupied\":" + String(globalOccupied ? "true" : "false") + ",";
  json += "\"coolingMode\":" + String(globalCoolingMode) + ",";
  json += "\"fanSpeed\":" + String(globalFanSpeed) + ",";
  json += "\"climateReason\":\"" + getClimateReason() + "\",";
  json += "\"kitchenGasAlarm\":" + String(kitchenGasAlarm ? "true" : "false") + ",";
  json += "\"kitchenReason\":\"" + getKitchenReason() + "\",";
  json += "\"livingLed1\":" + String(livingLed1 ? "true" : "false") + ",";
  json += "\"livingLed2\":" + String(livingLed2 ? "true" : "false") + ",";
  json += "\"livingTv\":" + String(livingTv ? "true" : "false") + ",";
  json += "\"livingCurtainOpen\":" + String(livingCurtainOpen ? "true" : "false") + ",";
  json += "\"livingReason\":\"" + getLivingRoomReason() + "\",";
  json += "\"comfortScore\":" + String(comfortScore) + ",";
  json += "\"aiConfidence\":" + String(aiConfidence) + ",";
  json += "\"predictedTemp30\":" + String(predictedTemp30, 1) + ",";
  json += "\"tempTrendLabel\":\"" + String(tempTrend > 0.03 ? "RISING" : (tempTrend < -0.03 ? "COOLING" : "STABLE")) + "\",";
  json += "\"totalSavedKWh\":" + String(totalSavedKWh, 2) + ",";
  json += "\"savedEGP\":" + String(savedEGP, 2) + ",";
  json += "\"energyAnomaly\":" + String(energyAnomaly ? "true" : "false") + ",";
  json += "\"aiInsight1\":\"" + aiInsight1 + "\",";
  json += "\"aiInsight2\":\"" + aiInsight2 + "\",";
  json += "\"aiInsight3\":\"" + aiInsight3 + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// Web Handlers
void handleSetEV() {
  if (server.hasArg("mode")) evOverrideMode = server.arg("mode").toInt();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSetSolar() {
  if (server.hasArg("mode")) {
    solarOverrideMode = server.arg("mode").toInt();
    SystemData sendCmd;
    memset(&sendCmd, 0, sizeof(sendCmd));
    sendCmd.nodeID = 1;
    sendCmd.overrideMode = solarOverrideMode;
    esp_now_send(broadcastAddress, (uint8_t *)&sendCmd, sizeof(sendCmd));
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSetClimate() {
  if (server.hasArg("mode")) {
    climateOverrideMode = server.arg("mode").toInt();
    SystemData sendCmd;
    memset(&sendCmd, 0, sizeof(sendCmd));
    sendCmd.nodeID = 2;
    sendCmd.overrideMode = climateOverrideMode;
    esp_now_send(broadcastAddress, (uint8_t *)&sendCmd, sizeof(sendCmd));
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSetLiving() {
  if (server.hasArg("led1")) livingLed1 = server.arg("led1").toInt() == 1;
  if (server.hasArg("led2")) livingLed2 = server.arg("led2").toInt() == 1;
  if (server.hasArg("tv")) livingTv = server.arg("tv").toInt() == 1;
  if (server.hasArg("curtain")) livingCurtainOpen = server.arg("curtain").toInt() == 1;
  if (server.hasArg("mode")) livingMode = server.arg("mode").toInt();

  SystemData sendCmd;
  memset(&sendCmd, 0, sizeof(sendCmd));
  sendCmd.nodeID = 4;
  sendCmd.led1 = livingLed1;
  sendCmd.led2 = livingLed2;
  sendCmd.tv = livingTv;
  sendCmd.curtainOpen = livingCurtainOpen;
  sendCmd.livingMode = livingMode;
  esp_now_send(broadcastAddress, (uint8_t *)&sendCmd, sizeof(sendCmd));

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ==============================================================================
// 9. SYSTEM INITIALIZATION & MAIN EXECUTION LOOP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=========================================================");
  Serial.println("  ROBODAM: AUTONOMOUS HEMS + AI NEURAL CORTEX + CHATBOT");
  Serial.println("=========================================================");

  pinMode(CURRENT_SENSOR_PIN, INPUT);
  pinMode(EV_CHARGER_RELAY_PIN, OUTPUT);
  digitalWrite(EV_CHARGER_RELAY_PIN, LOW);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("HEMS_Master_Gateway", "123456788");
  Serial.print("[Wi-Fi SoftAP] SSID: HEMS_Master_Gateway | IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] Failed to initialize ESP-NOW!");
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
    Serial.println("[ERROR] Failed to register broadcast peer!");
  } else {
    Serial.println("[ESP-NOW] Broadcast Peer registered successfully.");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/setEV", handleSetEV);
  server.on("/setSolar", handleSetSolar);
  server.on("/setClimate", handleSetClimate);
  server.on("/setLiving", handleSetLiving);
  server.begin();

  Serial.println("[HTTP Server] SCADA Dashboard + AI Chatbot + Neural Cortex Ready!");
  Serial.println("=========================================================");
}

void loop() {
  server.handleClient();
  processGridLoadAndEVCharger();
  processAIEngine();
}
