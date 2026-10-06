/* =========================================================================
   PROJECT: Smart Water Tank Controller (ESP32 + HC-SR04 + Relay + MQTT)
   -------------------------------------------------------------------------
   BROKER: 192.168.110.133 : 1883
   CREDENTIALS: rahat300809 / RAHAT678
   TELEMETRY TOPIC: devices/ESP32_WATER_01/telemetry
   COMMAND TOPIC:   devices/ESP32_WATER_01/command

   CORE ALGORITHMS & LOGIC (100% UNCHANGED):
   - 0.5s ultrasonic sampling (HC-SR04)
   - 2x Low-Pass Filtering (LPF1 alpha=0.35, LPF2 alpha=0.25)
   - 5.0s Statistical Window (Median + MAD + Tolerance Robust Mean)
   - Dual Hysteresis: Low Water 20s Confirmation ON / High Water 10s Confirmation OFF
   - Continuous verification and auto-cancellation if water level reverts
   - 10.0s Empty & Full Sensor Calibration routines
   - Non-volatile storage using ESP32 Preferences
   ========================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <math.h>

#define TRIG_PIN 5
#define ECHO_PIN 18
#define RELAY_PIN 25

/* ================= WIFI CREDENTIALS & HOTSPOT SETUP ================= */

const char* AP_SSID = "ESP32_Water_Setup";
const char* AP_PASS = "12345678";

// User Wi-Fi Credentials (Primary: WIFI / RAHAT)
const char* DEFAULT_WIFI_SSID = "WIFI";
const char* DEFAULT_WIFI_PASS = "RAHAT";

// Secondary Fallback Network
const char* FALLBACK_WIFI_SSID = "IoT Lab";
const char* FALLBACK_WIFI_PASS = "iot@diu123";

// User-Configured Wi-Fi loaded from Preferences
String configuredSSID = "";
String configuredPASS = "";


/* ================= MQTT CREDENTIALS & TOPICS ================= */

const char* MQTT_BROKER    = "192.168.110.133";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "rahat300809";
const char* MQTT_PASS      = "RAHAT678";
const char* MQTT_CLIENT_ID = "ESP32_WATER_01";

const char* TOPIC_TELEMETRY = "devices/ESP32_WATER_01/telemetry";
const char* TOPIC_COMMAND   = "devices/ESP32_WATER_01/command";

WiFiClient espClient;
PubSubClient mqtt(espClient);

DNSServer dnsServer;
WebServer server(80);
Preferences prefs;

/* ================= TIMING (ORIGINAL ALGO) ================= */

const unsigned long SAMPLE_INTERVAL  = 500;
const unsigned long NORMAL_WINDOW    = 5000;
const unsigned long CAL_TIME         = 10000;
const unsigned long ON_CONFIRM_TIME  = 20000;
const unsigned long OFF_CONFIRM_TIME = 10000;

/* ================= SETTINGS (ORIGINAL ALGO) ================= */

float tankRadius     = 50.0;
float tankHeight     = 100.0;

float motorOnPercent = 25.0;
float motorOffPercent = 75.0;

float sensorOffset   = 0.0;

/* ================= CALIBRATION (ORIGINAL ALGO) ================= */

float emptyDistance = 0;
float fullDistance  = 0;

bool calibrationReady   = false;
bool calibrationRunning = false;

String calibrationType = "";

unsigned long calibrationStart = 0;
unsigned long lastCalSample    = 0;

/* ================= SENSOR (ORIGINAL ALGO) ================= */

float rawDistance      = 0;
float filteredDistance = 0;

float lpf1 = 0;
float lpf2 = 0;

bool filterReady = false;

/* ================= NORMAL BUFFER (ORIGINAL ALGO) ================= */

#define NORMAL_MAX 15

float normalBuffer[NORMAL_MAX];
int normalCount = 0;

/* ================= CALIBRATION BUFFER (ORIGINAL ALGO) ================= */

#define CAL_MAX 25

float calBuffer[CAL_MAX];
int calCount = 0;

/* ================= CONFIRMATION BUFFER (ORIGINAL ALGO) ================= */

#define CONFIRM_MAX 50

float confirmBuffer[CONFIRM_MAX];
int confirmCount = 0;

/* ================= CONFIRMATION (ORIGINAL ALGO) ================= */

bool confirmationRunning   = false;
bool confirmingON          = false;
bool confirmationCancelled = false;

unsigned long confirmationStart = 0;

float confirmationLastPercent = 0;

/* ================= WATER (ORIGINAL ALGO) ================= */

float currentPercent = 0;
float currentLevel   = 0;
float currentLiters  = 0;
float tankCapacity   = 0;

float totalWaterUsed = 0;

float previousLevel        = 0;
bool previousLevelReady   = false;

/* ================= MOTOR (ORIGINAL ALGO) ================= */

bool relayState = false;
String motorMode = "AUTO";

/* ================= TIMERS ================= */

unsigned long lastSample            = 0;
unsigned long lastNormalCalculation = 0;
unsigned long lastMqttRetry         = 0;
unsigned long lastLiveMqttPublish   = 0;

/* Forward declaration */
void publishTelemetry();

/* =====================================================
   MOTOR CONTROL (ORIGINAL ALGO)
   ===================================================== */

void motorON() {
  digitalWrite(RELAY_PIN, LOW);
  relayState = true;

  Serial.println();
  Serial.println(">>> MOTOR ON");
  publishTelemetry();
}

void motorOFF() {
  digitalWrite(RELAY_PIN, HIGH);
  relayState = false;

  Serial.println();
  Serial.println(">>> MOTOR OFF");
  publishTelemetry();
}

/* =====================================================
   SAVE SETTINGS (ORIGINAL ALGO)
   ===================================================== */

void saveSettings() {
  prefs.begin("tank", false);
  prefs.putFloat("radius", tankRadius);
  prefs.putFloat("height", tankHeight);
  prefs.putFloat("on", motorOnPercent);
  prefs.putFloat("off", motorOffPercent);
  prefs.putFloat("offset", sensorOffset);
  prefs.end();

  prefs.begin("cal", false);
  prefs.putFloat("empty", emptyDistance);
  prefs.putFloat("full", fullDistance);
  prefs.end();

  prefs.begin("water", false);
  prefs.putFloat("used", totalWaterUsed);
  prefs.end();
}

/* =====================================================
   LOAD SETTINGS (ORIGINAL ALGO)
   ===================================================== */

void loadSettings() {
  prefs.begin("tank", true);
  tankRadius      = prefs.getFloat("radius", 50.0);
  tankHeight      = prefs.getFloat("height", 100.0);
  motorOnPercent  = prefs.getFloat("on", 25.0);
  motorOffPercent = prefs.getFloat("off", 75.0);
  sensorOffset    = prefs.getFloat("offset", 0.0);
  prefs.end();

  prefs.begin("cal", true);
  emptyDistance = prefs.getFloat("empty", 0);
  fullDistance  = prefs.getFloat("full", 0);
  prefs.end();

  prefs.begin("water", true);
  totalWaterUsed = prefs.getFloat("used", 0);
  prefs.end();

  calibrationReady =
    emptyDistance > fullDistance &&
    (emptyDistance - fullDistance) >= 5;
}

/* =====================================================
   LOAD WIFI SETTINGS
   ===================================================== */

void loadWiFiSettings() {
  prefs.begin("wifi_cfg", true);
  configuredSSID = prefs.getString("ssid", "");
  configuredPASS = prefs.getString("pass", "");
  prefs.end();
}

/* =====================================================
   RESET ALL (ORIGINAL ALGO)
   ===================================================== */

void resetAll() {
  prefs.begin("tank", false);
  prefs.clear();
  prefs.end();

  prefs.begin("cal", false);
  prefs.clear();
  prefs.end();

  prefs.begin("water", false);
  prefs.clear();
  prefs.end();

  prefs.begin("wifi_cfg", false);
  prefs.clear();
  prefs.end();
  configuredSSID = "";
  configuredPASS = "";

  tankRadius = 50;
  tankHeight = 100;

  motorOnPercent = 25;
  motorOffPercent = 75;

  sensorOffset = 0;

  emptyDistance = 0;
  fullDistance = 0;

  totalWaterUsed = 0;

  calibrationReady = false;

  motorMode = "AUTO";

  confirmationRunning = false;
  confirmationCancelled = false;

  normalCount = 0;
  confirmCount = 0;

  previousLevelReady = false;

  motorOFF();

  Serial.println();
  Serial.println(">>> ALL SETTINGS RESET");
  publishTelemetry();
}

/* =====================================================
   RESET WATER USAGE (ORIGINAL ALGO)
   ===================================================== */

void resetWaterUsed() {
  totalWaterUsed = 0;

  prefs.begin("water", false);
  prefs.putFloat("used", 0);
  prefs.end();

  Serial.println(">>> WATER USAGE RESET");
  publishTelemetry();
}

/* =====================================================
   HC-SR04 SENSOR READING (ORIGINAL ALGO)
   ===================================================== */

float readRawDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 35000);

  if (duration == 0) return -1;

  float d = duration * 0.0343 / 2.0;

  if (d < 2 || d > 400) return -1;

  return d;
}

/* =====================================================
   2x LOW PASS FILTER (ORIGINAL ALGO)
   ===================================================== */

float apply2XLPF(float value) {
  const float alpha1 = 0.35;
  const float alpha2 = 0.25;

  if (!filterReady) {
    lpf1 = value;
    lpf2 = value;
    filterReady = true;
    return lpf2;
  }

  lpf1 = lpf1 + alpha1 * (value - lpf1);
  lpf2 = lpf2 + alpha2 * (lpf1 - lpf2);

  return lpf2;
}

/* =====================================================
   SORTING (ORIGINAL ALGO)
   ===================================================== */

void sortValues(float* a, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (a[j] < a[i]) {
        float temp = a[i];
        a[i] = a[j];
        a[j] = temp;
      }
    }
  }
}

/* =====================================================
   MEDIAN VALUE (ORIGINAL ALGO)
   ===================================================== */

float medianValue(float* values, int n) {
  if (n <= 0) return 0;

  float temp[CONFIRM_MAX];
  for (int i = 0; i < n; i++) temp[i] = values[i];

  sortValues(temp, n);

  if (n % 2 == 0) return (temp[n / 2 - 1] + temp[n / 2]) / 2.0;
  return temp[n / 2];
}

/* =====================================================
   ROBUST MEAN (Median + MAD, ORIGINAL ALGO)
   ===================================================== */

float robustMean(float* values, int n, float &median, int &used) {
  if (n < 3) {
    median = 0;
    used = 0;
    return 0;
  }

  median = medianValue(values, n);

  float deviations[CONFIRM_MAX];
  for (int i = 0; i < n; i++) {
    deviations[i] = fabs(values[i] - median);
  }

  float mad = medianValue(deviations, n);
  float tolerance = 3.0 * mad;
  if (tolerance < 1.0) tolerance = 1.0;

  float sum = 0;
  used = 0;

  for (int i = 0; i < n; i++) {
    if (fabs(values[i] - median) <= tolerance) {
      sum += values[i];
      used++;
    }
  }

  if (used < 3) return median;
  return sum / used;
}

/* =====================================================
   DISTANCE -> PERCENTAGE (ORIGINAL ALGO)
   ===================================================== */

float distanceToPercent(float distance) {
  if (!calibrationReady) return 0;
  if (emptyDistance <= fullDistance) return 0;

  float percent = ((emptyDistance - distance) / (emptyDistance - fullDistance)) * 100.0;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  return percent;
}

/* =====================================================
   TANK GEOMETRY CALCULATION (ORIGINAL ALGO)
   ===================================================== */

void calculateTank() {
  tankCapacity = 3.14159265 * tankRadius * tankRadius * tankHeight / 1000.0;
  currentLevel = tankHeight * currentPercent / 100.0;
  currentLiters = tankCapacity * currentPercent / 100.0;
}

/* =====================================================
   WATER USAGE TRACKING (ORIGINAL ALGO)
   ===================================================== */

void updateWaterUsage() {
  if (!previousLevelReady) {
    previousLevel = currentLevel;
    previousLevelReady = true;
    return;
  }

  float decrease = previousLevel - currentLevel;
  if (decrease > 1.0) {
    float usedLiters = 3.14159265 * tankRadius * tankRadius * decrease / 1000.0;
    if (usedLiters > 0) {
      totalWaterUsed += usedLiters;

      prefs.begin("water", false);
      prefs.putFloat("used", totalWaterUsed);
      prefs.end();

      Serial.print("WATER USED +");
      Serial.print(usedLiters, 2);
      Serial.println(" L");
    }
  }

  previousLevel = currentLevel;
}

/* =====================================================
   START ON CONFIRMATION (ORIGINAL ALGO)
   ===================================================== */

void startONConfirmation() {
  if (confirmationRunning) return;

  confirmationRunning = true;
  confirmingON = true;
  confirmationCancelled = false;

  confirmationStart = millis();
  confirmCount = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("LOW WATER THRESHOLD REACHED");
  Serial.print("Current Water: ");
  Serial.print(currentPercent, 2);
  Serial.println("%");
  Serial.println("START 20 SECOND CONFIRMATION");
  Serial.println("MOTOR STAYS OFF");
  Serial.println("================================");

  publishTelemetry();
}

/* =====================================================
   START OFF CONFIRMATION (ORIGINAL ALGO)
   ===================================================== */

void startOFFConfirmation() {
  if (confirmationRunning) return;

  confirmationRunning = true;
  confirmingON = false;
  confirmationCancelled = false;

  confirmationStart = millis();
  confirmCount = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("HIGH WATER THRESHOLD REACHED");
  Serial.print("Current Water: ");
  Serial.print(currentPercent, 2);
  Serial.println("%");
  Serial.println("START 10 SECOND CONFIRMATION");
  Serial.println("MOTOR STAYS ON");
  Serial.println("================================");

  publishTelemetry();
}

/* =====================================================
   CONFIRMATION PROCESS (ORIGINAL ALGO)
   ===================================================== */

void processConfirmation() {
  if (!confirmationRunning) return;

  unsigned long elapsed = millis() - confirmationStart;
  unsigned long requiredTime = confirmingON ? ON_CONFIRM_TIME : OFF_CONFIRM_TIME;

  if (confirmCount >= 5) {
    float tempMedian = 0;
    int tempUsed = 0;

    float tempDistance = robustMean(confirmBuffer, confirmCount, tempMedian, tempUsed);

    if (tempDistance > 0) {
      float tempPercent = distanceToPercent(tempDistance);
      confirmationLastPercent = tempPercent;

      if (confirmingON && tempPercent > motorOnPercent) {
        confirmationCancelled = true;
        Serial.println();
        Serial.println(">>> ON CONFIRMATION CANCELLED");
        Serial.print("Water returned to ");
        Serial.print(tempPercent, 2);
        Serial.println("%");
      }

      if (!confirmingON && tempPercent < motorOffPercent) {
        confirmationCancelled = true;
        Serial.println();
        Serial.println(">>> OFF CONFIRMATION CANCELLED");
        Serial.print("Water returned to ");
        Serial.print(tempPercent, 2);
        Serial.println("%");
      }
    }
  }

  if (confirmationCancelled) {
    confirmationRunning = false;
    confirmCount = 0;

    Serial.println(">>> CONFIRMATION RESET");

    if (!confirmingON) {
      motorON();
      Serial.println(">>> MOTOR REMAINS ON");
    }

    publishTelemetry();
    return;
  }

  if (elapsed < requiredTime) return;

  confirmationRunning = false;

  if (confirmCount < 5) {
    Serial.println(">>> CONFIRMATION FAILED");
    Serial.println(">>> NOT ENOUGH DATA");
    if (!confirmingON) motorON();
    confirmCount = 0;
    publishTelemetry();
    return;
  }

  float median = 0;
  int used = 0;

  float finalDistance = robustMean(confirmBuffer, confirmCount, median, used);
  float finalPercent  = distanceToPercent(finalDistance);

  Serial.println();
  Serial.println("================================");
  if (confirmingON) Serial.println("20 SECOND MOTOR ON RESULT");
  else              Serial.println("10 SECOND MOTOR OFF RESULT");

  Serial.print("Samples: ");       Serial.println(confirmCount);
  Serial.print("Median: ");        Serial.print(median, 2); Serial.println(" cm");
  Serial.print("Used Samples: ");  Serial.println(used);
  Serial.print("Final Distance: ");Serial.print(finalDistance, 2); Serial.println(" cm");
  Serial.print("Final Water: ");   Serial.print(finalPercent, 2); Serial.println("%");

  if (confirmingON) {
    if (finalPercent <= motorOnPercent) {
      Serial.println(">>> 20 SEC CONFIRMED");
      Serial.println(">>> LOW WATER CONFIRMED");
      motorON();
    } else {
      Serial.println(">>> LOW WATER NOT CONFIRMED");
      Serial.println(">>> MOTOR REMAINS OFF");
    }
  } else {
    if (finalPercent >= motorOffPercent) {
      Serial.println(">>> 10 SEC CONFIRMED");
      Serial.println(">>> HIGH WATER CONFIRMED");
      motorOFF();
    } else {
      Serial.println(">>> HIGH WATER NOT CONFIRMED");
      Serial.println(">>> MOTOR REMAINS ON");
      motorON();
    }
  }

  Serial.println("================================");
  confirmCount = 0;
  publishTelemetry();
}

/* =====================================================
   AUTO MOTOR CONTROL (ORIGINAL ALGO)
   ===================================================== */

void autoMotorControl() {
  if (motorMode != "AUTO") return;

  if (!calibrationReady) {
    motorOFF();
    return;
  }

  if (confirmationRunning) return;

  if (!relayState && currentPercent <= motorOnPercent) {
    startONConfirmation();
    return;
  }

  if (relayState && currentPercent >= motorOffPercent) {
    startOFFConfirmation();
    return;
  }
}

/* =====================================================
   NORMAL 5-SECOND DATA PROCESS (ORIGINAL ALGO)
   ===================================================== */

void processNormalData() {
  if (normalCount < 5) return;

  float median = 0;
  int used = 0;

  float result = robustMean(normalBuffer, normalCount, median, used);
  if (result <= 0) return;

  filteredDistance = result;
  currentPercent = distanceToPercent(filteredDistance);
  calculateTank();
  updateWaterUsage();

  Serial.println();
  Serial.println("================================");
  Serial.println("5 SECOND FINAL DATA");
  Serial.println("================================");
  Serial.print("Samples: ");       Serial.println(normalCount);
  Serial.print("Median: ");        Serial.print(median, 2); Serial.println(" cm");
  Serial.print("Used Samples: ");  Serial.println(used);
  Serial.print("Final Distance: ");Serial.print(filteredDistance, 2); Serial.println(" cm");
  Serial.print("Water: ");         Serial.print(currentPercent, 2); Serial.println("%");
  Serial.print("Level: ");         Serial.print(currentLevel, 2); Serial.println(" cm");
  Serial.print("Liters: ");        Serial.print(currentLiters, 2); Serial.println(" L");
  Serial.println("================================");

  autoMotorControl();
  normalCount = 0;

  // Transmit fresh 5s telemetry to MQTT broker
  publishTelemetry();
}

/* =====================================================
   SENSOR SAMPLING (0.5 SEC, ORIGINAL ALGO)
   ===================================================== */

void sampleSensor() {
  float raw = readRawDistance();

  if (raw <= 0) {
    Serial.println("HC-SR04 INVALID READING");
    return;
  }

  raw += sensorOffset;
  float smooth = apply2XLPF(raw);
  rawDistance = raw;

  if (normalCount < NORMAL_MAX) {
    normalBuffer[normalCount] = smooth;
    normalCount++;
  }

  if (confirmationRunning && confirmCount < CONFIRM_MAX) {
    confirmBuffer[confirmCount] = smooth;
    confirmCount++;
  }

  Serial.print("RAW=");
  Serial.print(raw, 2);
  Serial.print(" | LPF1=");
  Serial.print(lpf1, 2);
  Serial.print(" | LPF2=");
  Serial.print(lpf2, 2);
  Serial.println(" cm");
}

/* =====================================================
   START CALIBRATION (ORIGINAL ALGO)
   ===================================================== */

void startCalibration(String type) {
  if (calibrationRunning) return;

  motorOFF();

  confirmationRunning = false;
  confirmationCancelled = false;
  confirmCount = 0;

  calibrationRunning = true;
  calibrationType = type;
  calibrationStart = millis();
  lastCalSample = millis() - SAMPLE_INTERVAL;
  calCount = 0;

  filterReady = false;
  lpf1 = 0;
  lpf2 = 0;

  Serial.println();
  Serial.println("================================");
  Serial.print("CALIBRATION START: ");
  Serial.println(type);
  Serial.println("KEEP TANK STEADY");
  Serial.println("COLLECTING 10 SECONDS");
  Serial.println("================================");

  publishTelemetry();
}

/* =====================================================
   CALIBRATION PROCESS (ORIGINAL ALGO)
   ===================================================== */

void processCalibration() {
  if (!calibrationRunning) return;

  unsigned long elapsed = millis() - calibrationStart;

  if (millis() - lastCalSample >= SAMPLE_INTERVAL) {
    lastCalSample = millis();

    float raw = readRawDistance();
    if (raw > 0) {
      raw += sensorOffset;
      float smooth = apply2XLPF(raw);

      if (calCount < CAL_MAX) {
        calBuffer[calCount] = smooth;
        calCount++;
      }

      Serial.print("CAL ");
      Serial.print(elapsed / 1000);
      Serial.print("s | RAW=");
      Serial.print(raw, 2);
      Serial.print(" | LPF2=");
      Serial.println(smooth, 2);
    }
  }

  if (elapsed >= CAL_TIME) {
    if (calCount >= 5) {
      float median = 0;
      int used = 0;

      float result = robustMean(calBuffer, calCount, median, used);

      if (calibrationType == "empty") emptyDistance = result;
      if (calibrationType == "full")  fullDistance = result;

      calibrationReady =
        emptyDistance > fullDistance &&
        (emptyDistance - fullDistance) >= 5;

      prefs.begin("cal", false);
      prefs.putFloat("empty", emptyDistance);
      prefs.putFloat("full", fullDistance);
      prefs.end();

      Serial.println();
      Serial.println("================================");
      Serial.println("CALIBRATION COMPLETE");
      Serial.print("Samples: ");          Serial.println(calCount);
      Serial.print("Median: ");           Serial.print(median, 2); Serial.println(" cm");
      Serial.print("Used: ");             Serial.println(used);
      Serial.print("Final: ");            Serial.print(result, 2); Serial.println(" cm");
      Serial.print("Empty: ");            Serial.println(emptyDistance, 2);
      Serial.print("Full: ");             Serial.println(fullDistance, 2);
      Serial.print("Calibration Ready: ");Serial.println(calibrationReady ? "YES" : "NO");
      Serial.println("================================");
    } else {
      Serial.println("CALIBRATION FAILED");
      Serial.println("Not enough valid samples");
    }

    calibrationRunning = false;
    calCount = 0;
    filterReady = false;
    normalCount = 0;

    publishTelemetry();
  }
}

/* =====================================================
   BUILD TELEMETRY JSON
   Produces identical JSON for both WebServer & MQTT
   ===================================================== */

String buildTelemetryJSON() {
  String state = "NORMAL";
  unsigned long remaining = 0;
  float progress = 0;

  if (calibrationRunning) {
    state = "CALIBRATING";
    unsigned long elapsed = millis() - calibrationStart;
    if (elapsed < CAL_TIME) {
      remaining = (CAL_TIME - elapsed) / 1000;
      progress = (float(elapsed) / CAL_TIME) * 100.0;
      if (progress > 100) progress = 100;
    }
  } else if (confirmationRunning) {
    state = confirmingON ? "VERIFY_ON" : "VERIFY_OFF";
    unsigned long required = confirmingON ? ON_CONFIRM_TIME : OFF_CONFIRM_TIME;
    unsigned long elapsed = millis() - confirmationStart;
    if (elapsed < required) {
      remaining = (required - elapsed) / 1000;
      progress = (float(elapsed) / required) * 100.0;
      if (progress > 100) progress = 100;
    }
  }

  String json = "{";
  json += "\"device_id\":\"ESP32_WATER_01\",";
  json += "\"distance\":" + String(filteredDistance, 2) + ",";
  json += "\"raw_distance\":" + String(rawDistance, 2) + ",";
  json += "\"level\":" + String(currentLevel, 2) + ",";
  json += "\"water_level\":" + String(currentLevel, 2) + ",";
  json += "\"percent\":" + String(currentPercent, 2) + ",";
  json += "\"water_percent\":" + String(currentPercent, 2) + ",";
  json += "\"liters\":" + String(currentLiters, 2) + ",";
  json += "\"water_liters\":" + String(currentLiters, 2) + ",";
  json += "\"capacity\":" + String(tankCapacity, 2) + ",";
  json += "\"tank_capacity\":" + String(tankCapacity, 2) + ",";
  json += "\"used\":" + String(totalWaterUsed, 2) + ",";
  json += "\"water_used\":" + String(totalWaterUsed, 2) + ",";
  json += "\"radius\":" + String(tankRadius, 2) + ",";
  json += "\"tank_radius\":" + String(tankRadius, 2) + ",";
  json += "\"height\":" + String(tankHeight, 2) + ",";
  json += "\"tank_height\":" + String(tankHeight, 2) + ",";
  json += "\"on\":" + String(motorOnPercent, 1) + ",";
  json += "\"motor_on_percent\":" + String(motorOnPercent, 1) + ",";
  json += "\"off\":" + String(motorOffPercent, 1) + ",";
  json += "\"motor_off_percent\":" + String(motorOffPercent, 1) + ",";
  json += "\"offset\":" + String(sensorOffset, 2) + ",";
  json += "\"sensor_offset\":" + String(sensorOffset, 2) + ",";
  json += "\"empty\":" + String(emptyDistance, 2) + ",";
  json += "\"empty_distance\":" + String(emptyDistance, 2) + ",";
  json += "\"full\":" + String(fullDistance, 2) + ",";
  json += "\"full_distance\":" + String(fullDistance, 2) + ",";
  json += "\"calibrated\":" + String(calibrationReady ? "true" : "false") + ",";
  json += "\"calibrationRunning\":" + String(calibrationRunning ? "true" : "false") + ",";
  json += "\"calibration_running\":" + String(calibrationRunning ? "true" : "false") + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"motor\":" + String(relayState ? "true" : "false") + ",";
  json += "\"motor_state\":\"" + String(relayState ? "ON" : "OFF") + "\",";
  json += "\"mode\":\"" + motorMode + "\",";
  json += "\"motor_mode\":\"" + motorMode + "\",";
  json += "\"state\":\"" + state + "\",";
  json += "\"system_state\":\"" + state + "\",";
  json += "\"remaining\":" + String(remaining) + ",";
  json += "\"calType\":\"" + calibrationType + "\",";
  json += "\"progress\":" + String(progress, 1) + ",";
  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifi_ssid\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "") + "\",";
  json += "\"wifi_ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\"";
  json += "}";

  return json;
}

/* =====================================================
   MQTT PUBLISHER
   ===================================================== */

void publishTelemetry() {
  if (mqtt.connected()) {
    String payload = buildTelemetryJSON();
    mqtt.publish(TOPIC_TELEMETRY, payload.c_str());
  }
}

/* =====================================================
   MQTT COMMAND PARSER & CALLBACK
   Receives commands from the web UI and executes them
   ===================================================== */

// Helper to extract JSON string value: "key":"value"
String extractJsonString(String payload, String key) {
  int keyIndex = payload.indexOf("\"" + key + "\"");
  if (keyIndex == -1) return "";
  int colonIndex = payload.indexOf(":", keyIndex);
  if (colonIndex == -1) return "";
  int startQuote = payload.indexOf("\"", colonIndex);
  if (startQuote == -1) return "";
  int endQuote = payload.indexOf("\"", startQuote + 1);
  if (endQuote == -1) return "";
  return payload.substring(startQuote + 1, endQuote);
}

// Helper to extract JSON numeric value: "key":123.4
float extractJsonFloat(String payload, String key, float defaultVal) {
  int keyIndex = payload.indexOf("\"" + key + "\"");
  if (keyIndex == -1) return defaultVal;
  int colonIndex = payload.indexOf(":", keyIndex);
  if (colonIndex == -1) return defaultVal;
  int start = colonIndex + 1;
  while (start < payload.length() && (payload[start] == ' ' || payload[start] == '"')) start++;
  int end = start;
  while (end < payload.length() && payload[end] != ',' && payload[end] != '}' && payload[end] != '"' && payload[end] != ' ') end++;
  if (start >= end) return defaultVal;
  return payload.substring(start, end).toFloat();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.println();
  Serial.print(">>> MQTT MESSAGE RECEIVED on [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  String action = extractJsonString(message, "action");
  if (action == "") action = extractJsonString(message, "command");
  action.toLowerCase();

  String state = extractJsonString(message, "state");
  if (state == "") state = extractJsonString(message, "value");
  state.toLowerCase();

  /* --- 1. MOTOR & MODE COMMANDS --- */
  if (action == "motor" || action == "mode" || message.startsWith("motor:")) {
    if (state == "" && message.indexOf(":") != -1) {
      state = message.substring(message.indexOf(":") + 1);
      state.toLowerCase();
    }

    if (state == "on") {
      motorMode = "MANUAL";
      confirmationRunning = false;
      confirmationCancelled = false;
      confirmCount = 0;
      motorON();
    } else if (state == "off") {
      motorMode = "MANUAL";
      confirmationRunning = false;
      confirmationCancelled = false;
      confirmCount = 0;
      motorOFF();
    } else if (state == "auto") {
      motorMode = "AUTO";
      confirmationRunning = false;
      confirmationCancelled = false;
      confirmCount = 0;
      Serial.println(">>> AUTO MODE ENABLED VIA MQTT");
      if (!calibrationReady) {
        motorOFF();
      }
      publishTelemetry();
    }
  }

  /* --- 2. SAVE SETTINGS COMMAND --- */
  else if (action == "save" || action == "settings") {
    float newRadius = extractJsonFloat(message, "radius", -1);
    if (newRadius < 0) newRadius = extractJsonFloat(message, "tank_radius", tankRadius);

    float newHeight = extractJsonFloat(message, "height", -1);
    if (newHeight < 0) newHeight = extractJsonFloat(message, "tank_height", tankHeight);

    float newOn = extractJsonFloat(message, "on", -1);
    if (newOn < 0) newOn = extractJsonFloat(message, "motor_on_percent", motorOnPercent);

    float newOff = extractJsonFloat(message, "off", -1);
    if (newOff < 0) newOff = extractJsonFloat(message, "motor_off_percent", motorOffPercent);

    float newOffset = extractJsonFloat(message, "offset", -999);
    if (newOffset == -999) newOffset = extractJsonFloat(message, "sensor_offset", sensorOffset);

    if (newRadius > 0 && newHeight > 0 && newOn < newOff) {
      tankRadius     = newRadius;
      tankHeight     = newHeight;
      motorOnPercent = newOn;
      motorOffPercent= newOff;
      sensorOffset   = newOffset;

      saveSettings();
      calculateTank();
      Serial.println(">>> MQTT: SETTINGS SAVED");
      publishTelemetry();
    } else {
      Serial.println(">>> MQTT: INVALID SETTINGS PARAMETERS");
    }
  }

  /* --- 3. CALIBRATION COMMAND --- */
  else if (action == "calibrate" || message.startsWith("calibrate:")) {
    String type = extractJsonString(message, "type");
    if (type == "" && message.indexOf(":") != -1) {
      type = message.substring(message.indexOf(":") + 1);
    }
    type.toLowerCase();

    if (type == "empty" || type == "full") {
      startCalibration(type);
    }
  }

  /* --- 4. RESET USAGE COMMAND --- */
  else if (action == "resetusage" || action == "reset_usage" || message == "resetUsage") {
    resetWaterUsed();
  }

  /* --- 5. RESET ALL COMMAND --- */
  else if (action == "resetall" || action == "reset_all" || message == "resetAll") {
    resetAll();
  }

  /* --- 6. STATUS / PING REQUEST --- */
  else if (action == "status" || action == "getstatus" || message == "status") {
    publishTelemetry();
  }
}

/* =====================================================
   NON-BLOCKING MQTT CONNECTION HANDLER
   ===================================================== */

void handleMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttRetry >= 4000) {
    lastMqttRetry = now;

    Serial.print("Attempting MQTT connection to ");
    Serial.print(MQTT_BROKER);
    Serial.print("...");

    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" CONNECTED!");
      mqtt.subscribe(TOPIC_COMMAND);
      publishTelemetry();
    } else {
      Serial.print(" FAILED, rc=");
      Serial.println(mqtt.state());
    }
  }
}

/* =====================================================
   EMBEDDED WEB SERVER API ENDPOINTS (ORIGINAL)
   ===================================================== */

void sendData() {
  String json = buildTelemetryJSON();
  server.send(200, "application/json", json);
}

void saveAPI() {
  if (server.hasArg("radius")) tankRadius = server.arg("radius").toFloat();
  if (server.hasArg("height")) tankHeight = server.arg("height").toFloat();
  if (server.hasArg("on"))     motorOnPercent = server.arg("on").toFloat();
  if (server.hasArg("off"))    motorOffPercent = server.arg("off").toFloat();
  if (server.hasArg("offset")) sensorOffset = server.arg("offset").toFloat();

  if (tankRadius <= 0 || tankHeight <= 0) {
    server.send(400, "text/plain", "Invalid tank dimensions");
    return;
  }

  if (motorOnPercent >= motorOffPercent) {
    server.send(400, "text/plain", "Motor OFF percentage must be greater than Motor ON percentage.");
    return;
  }

  saveSettings();
  calculateTank();
  publishTelemetry();
  server.send(200, "text/plain", "Settings saved successfully");
}

void motorAPI() {
  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "Missing state");
    return;
  }

  String state = server.arg("state");

  if (state == "on") {
    motorMode = "MANUAL";
    confirmationRunning = false;
    confirmationCancelled = false;
    confirmCount = 0;
    motorON();
    server.send(200, "text/plain", "Manual Motor ON");
    return;
  }

  if (state == "off") {
    motorMode = "MANUAL";
    confirmationRunning = false;
    confirmationCancelled = false;
    confirmCount = 0;
    motorOFF();
    server.send(200, "text/plain", "Manual Motor OFF");
    return;
  }

  if (state == "auto") {
    motorMode = "AUTO";
    confirmationRunning = false;
    confirmationCancelled = false;
    confirmCount = 0;
    Serial.println(">>> AUTO MODE ENABLED");

    if (!calibrationReady) {
      motorOFF();
      server.send(200, "text/plain", "AUTO enabled. Please calibrate first.");
      return;
    }
    publishTelemetry();
    server.send(200, "text/plain", "AUTO mode enabled");
    return;
  }

  server.send(400, "text/plain", "Invalid motor state");
}

void calibrationAPI() {
  if (!server.hasArg("type")) {
    server.send(400, "text/plain", "Missing calibration type");
    return;
  }

  if (calibrationRunning) {
    server.send(200, "text/plain", "Calibration already running");
    return;
  }

  String type = server.arg("type");
  if (type != "empty" && type != "full") {
    server.send(400, "text/plain", "Invalid calibration type");
    return;
  }

  startCalibration(type);
  server.send(200, "text/plain", "10 second calibration started. Keep tank steady.");
}

void resetUsageAPI() {
  resetWaterUsed();
  server.send(200, "text/plain", "Water usage reset");
}

void resetAllAPI() {
  resetAll();
  server.send(200, "text/plain", "All settings reset successfully");
}

/* =====================================================
   WIFI MANAGER WEB PORTAL & CAPTIVE PORTAL
   ===================================================== */

void handleCaptiveRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/wifi", true);
  server.send(302, "text/plain", "");
}

void handleWifiPage() {
  int n = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>ESP32 Wi-Fi Setup</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f172a;color:#f8fafc;padding:16px;}";
  html += ".card{max-width:440px;margin:10px auto;background:#1e293b;padding:24px;border-radius:18px;box-shadow:0 10px 25px rgba(0,0,0,0.5);border:1px solid #334155;}";
  html += "h2{margin:0 0 6px 0;font-size:22px;color:#38bdf8;}";
  html += "p{color:#94a3b8;font-size:14px;margin:0 0 16px 0;}";
  html += ".status{padding:12px;border-radius:10px;margin-bottom:16px;font-size:13px;font-weight:600;}";
  html += ".online{background:rgba(34,197,94,0.15);color:#4ade80;border:1px solid rgba(34,197,94,0.3);}";
  html += ".offline{background:rgba(239,68,68,0.15);color:#f87171;border:1px solid rgba(239,68,68,0.3);}";
  html += ".net-list{margin-bottom:18px;max-height:200px;overflow-y:auto;border:1px solid #334155;border-radius:12px;padding:4px;background:#0f172a;}";
  html += ".net-item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;border-radius:8px;cursor:pointer;border-bottom:1px solid #1e293b;}";
  html += ".net-item:hover{background:#334155;}";
  html += ".net-name{font-weight:600;font-size:14px;color:#f1f5f9;}";
  html += ".net-meta{font-size:12px;color:#94a3b8;}";
  html += "label{display:block;font-size:12px;font-weight:700;color:#94a3b8;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:6px;}";
  html += "input{width:100%;padding:12px 14px;border:1px solid #475569;border-radius:10px;background:#0f172a;color:white;font-size:15px;margin-bottom:14px;outline:none;}";
  html += "input:focus{border-color:#38bdf8;}";
  html += ".btn{width:100%;background:#0284c7;color:white;border:0;padding:14px;border-radius:12px;font-size:15px;font-weight:700;cursor:pointer;}";
  html += ".btn:hover{background:#0369a1;}";
  html += ".dash-btn{display:block;text-align:center;margin-top:14px;color:#38bdf8;text-decoration:none;font-size:14px;font-weight:600;}";
  html += "</style>";
  html += "<script>";
  html += "function sel(ssid){document.getElementById('ssid').value=ssid;document.getElementById('pass').focus();}";
  html += "</script>";
  html += "</head><body><div class=\"card\">";
  html += "<h2>📶 Wi-Fi Setup</h2>";
  html += "<p>Select your Wi-Fi network below or enter manually.</p>";

  if (WiFi.status() == WL_CONNECTED) {
    html += "<div class=\"status online\">✓ Connected to <b>" + WiFi.SSID() + "</b><br>IP: " + WiFi.localIP().toString() + "</div>";
  } else {
    html += "<div class=\"status offline\">⚠ Disconnected — Hotspot Mode Active<br>Hotspot: <b>" + String(AP_SSID) + "</b></div>";
  }

  html += "<label>Discovered Networks (" + String(n > 0 ? n : 0) + ")</label>";
  html += "<div class=\"net-list\">";
  if (n <= 0) {
    html += "<div style=\"padding:12px;color:#94a3b8;text-align:center;\">No networks found. Try refreshing.</div>";
  } else {
    for (int i = 0; i < n; ++i) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      int r = WiFi.RSSI(i);
      int pct = constrain(2 * (r + 100), 0, 100);
      bool enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      html += "<div class=\"net-item\" onclick=\"sel('" + s + "')\">";
      html += "<span class=\"net-name\">" + s + "</span>";
      html += "<span class=\"net-meta\">" + String(pct) + "% " + (enc ? "🔒" : "🔓") + "</span>";
      html += "</div>";
    }
  }
  html += "</div>";

  String defSSID = configuredSSID != "" ? configuredSSID : String(DEFAULT_WIFI_SSID);
  html += "<form method=\"POST\" action=\"/wifisave\">";
  html += "<label for=\"ssid\">Network Name (SSID)</label>";
  html += "<input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"e.g. MyWiFi\" value=\"" + defSSID + "\" required>";
  html += "<label for=\"pass\">Wi-Fi Password</label>";
  html += "<input type=\"password\" id=\"pass\" name=\"pass\" placeholder=\"Enter password\">";
  html += "<button type=\"submit\" class=\"btn\">Save & Connect</button>";
  html += "</form>";
  html += "<a href=\"/\" class=\"dash-btn\">← Tank Controller Dashboard</a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleWifiSave() {
  String newSSID = server.arg("ssid");
  String newPASS = server.arg("pass");
  newSSID.trim();
  newPASS.trim();

  if (newSSID.length() == 0) {
    server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }

  // Persist to Preferences
  prefs.begin("wifi_cfg", false);
  prefs.putString("ssid", newSSID);
  prefs.putString("pass", newPASS);
  prefs.end();

  configuredSSID = newSSID;
  configuredPASS = newPASS;

  String html = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<meta http-equiv=\"refresh\" content=\"10; url=/wifi\">";
  html += "<title>Connecting...</title>";
  html += "<style>body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f172a;color:#f8fafc;padding:30px;text-align:center;}";
  html += ".card{max-width:400px;margin:auto;background:#1e293b;padding:30px;border-radius:18px;border:1px solid #334155;}";
  html += ".spinner{border:4px solid #334155;border-top:4px solid #38bdf8;border-radius:50%;width:44px;height:44px;animation:spin 1s linear infinite;margin:20px auto;}";
  html += "@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}";
  html += "a{display:inline-block;margin-top:16px;color:#38bdf8;text-decoration:none;font-weight:600;}";
  html += "</style></head><body><div class=\"card\">";
  html += "<h2 style=\"color:#38bdf8;margin-top:0;\">Connecting to Wi-Fi</h2>";
  html += "<p>Applying credentials for <b>" + newSSID + "</b>...</p>";
  html += "<div class=\"spinner\"></div>";
  html += "<p style=\"color:#94a3b8;font-size:13px;\">Please wait ~10 seconds. If successful, ESP32 will connect and reach the MQTT broker.</p>";
  html += "<a href=\"/wifi\">← Back to Setup</a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);

  Serial.println();
  Serial.print(">>> Saving and connecting to Wi-Fi: ");
  Serial.println(newSSID);

  WiFi.disconnect();
  delay(300);
  WiFi.begin(newSSID.c_str(), newPASS.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(400);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(">>> Successfully connected to: ");
    Serial.println(newSSID);
    Serial.print(">>> IP Address: ");
    Serial.println(WiFi.localIP());
    publishTelemetry();
  } else {
    Serial.println();
    Serial.println(">>> Connection failed or timed out. AP hotspot remains active.");
  }
}

/* =====================================================
   WIFI CONNECTION ROUTINE (MULTI-NETWORK + PREFERENCES)
   ===================================================== */

void attemptWiFiConnection() {
  bool connected = false;

  // 1. Try user-configured Wi-Fi if available
  if (configuredSSID.length() > 0) {
    Serial.print("Connecting to Configured Wi-Fi: ");
    Serial.print(configuredSSID);
    Serial.print(" ... ");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(configuredSSID.c_str(), configuredPASS.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
      delay(400);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    }
    Serial.println();
  }

  // 2. Try Primary Wi-Fi (WIFI / RAHAT)
  if (!connected) {
    Serial.print("Connecting to Primary Wi-Fi (");
    Serial.print(DEFAULT_WIFI_SSID);
    Serial.print(") ... ");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
      delay(400);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    }
    Serial.println();
  }

  // 3. Try Fallback Wi-Fi (IoT Lab)
  if (!connected) {
    Serial.print("Connecting to Fallback Wi-Fi (");
    Serial.print(FALLBACK_WIFI_SSID);
    Serial.print(") ... ");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(FALLBACK_WIFI_SSID, FALLBACK_WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 6000) {
      delay(400);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    }
    Serial.println();
  }

  if (connected) {
    Serial.println("====================================");
    Serial.print(">>> WiFi Connected! IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("====================================");
  } else {
    Serial.println("====================================");
    Serial.println("[!] Wi-Fi Connection FAILED / TIMED OUT");
    Serial.println("[!] ESP32 HOTSPOT ACTIVE:");
    Serial.print("    SSID:          "); Serial.println(AP_SSID);
    Serial.print("    Password:      "); Serial.println(AP_PASS);
    Serial.println("    Web Setup URL: http://192.168.4.1/wifi");
    Serial.println("[!] Connect your phone/laptop to configure any Wi-Fi!");
    Serial.println("====================================");
  }
}

/* =====================================================
   EMBEDDED HTML WEB INTERFACE (LOCAL AP / WIFI ACCESS)
   ===================================================== */

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Water Tank Controller</title>
<style>
*{box-sizing:border-box;}
body{margin:0;font-family:Arial,sans-serif;background:#f4f7fb;color:#172033;}
.container{max-width:1100px;margin:auto;padding:20px;}
.header{background:white;padding:20px;border-radius:18px;box-shadow:0 4px 15px #dce3ed;margin-bottom:20px;}
.header h1{margin:0;font-size:28px;}
.header p{color:#64748b;margin-bottom:0;}
.card{background:white;padding:20px;border-radius:18px;box-shadow:0 4px 15px #dce3ed;margin-bottom:20px;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:15px;}
.stat{background:#f8fafc;border:1px solid #e2e8f0;padding:18px;border-radius:14px;}
.label{font-size:13px;color:#64748b;}
.value{font-size:25px;font-weight:bold;margin-top:6px;}
.tankWrapper{display:flex;justify-content:center;align-items:center;padding:30px;}
.tank{width:220px;height:400px;border:6px solid #334155;border-radius:20px;position:relative;overflow:visible;background:#e2e8f0;}
.water{position:absolute;bottom:0;left:0;width:100%;height:0%;background:linear-gradient(to top,#0284c7,#38bdf8);transition:height .15s linear;}
.waterText{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);font-size:32px;font-weight:bold;z-index:5;}
.marker{position:absolute;left:230px;font-size:13px;color:#64748b;}
.m25{bottom:25%;} .m50{bottom:50%;} .m75{bottom:75%;} .m100{bottom:97%;}
.motorBox{text-align:center;padding:15px;border-radius:14px;background:#f8fafc;}
.motorStatus{font-size:34px;font-weight:bold;margin:10px;}
.motorOn{color:#16a34a;} .motorOff{color:#dc2626;}
.mode{font-size:15px;font-weight:bold;color:#475569;}
button{border:0;padding:12px 18px;margin:5px;border-radius:9px;cursor:pointer;font-weight:bold;}
.green{background:#16a34a;color:white;} .red{background:#dc2626;color:white;} .blue{background:#2563eb;color:white;}
.purple{background:#7c3aed;color:white;} .gray{background:#64748b;color:white;} .orange{background:#ea580c;color:white;}
input{width:100%;padding:12px;border:1px solid #cbd5e1;border-radius:9px;font-size:16px;margin-top:6px;margin-bottom:14px;}
.progress{height:12px;background:#e2e8f0;border-radius:10px;overflow:hidden;margin-top:10px;}
.progressBar{height:100%;width:0%;background:#7c3aed;transition:width .5s;}
.notice{padding:15px;border-radius:10px;background:#eff6ff;color:#1d4ed8;font-weight:bold;text-align:center;}
.small{font-size:13px;color:#64748b;}
.sectionTitle{margin-top:0;}
.center{text-align:center;}
.threshold{padding:12px;background:#f8fafc;border-radius:10px;margin-top:10px;}
</style>
</head>
<body>
<div class="container">
<div class="header">
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;">
    <div>
      <h1 style="margin:0;">💧 Smart Water Tank Controller</h1>
      <p style="margin:5px 0 0 0;color:#64748b;">ESP32 • HC-SR04 • 2× LPF • 5 Second Statistical Filtering • MQTT Ready</p>
    </div>
    <div>
      <a href="/wifi" style="display:inline-block;background:#0284c7;color:white;padding:10px 16px;border-radius:10px;text-decoration:none;font-weight:bold;font-size:14px;">📶 Wi-Fi Settings</a>
    </div>
  </div>
</div>
<div class="card">
<h2 class="sectionTitle">⚡ Motor Control</h2>
<div class="motorBox">
<div id="motorStatus" class="motorStatus motorOff">OFF</div>
<div class="mode">Mode: <strong id="motorMode">AUTO</strong></div>
<div id="systemState" class="notice">NORMAL</div>
<div id="countdown" class="small"></div>
</div>
<div class="center">
<button class="green" onclick="motor('on')">MANUAL ON</button>
<button class="red" onclick="motor('off')">MANUAL OFF</button>
<button class="blue" onclick="motor('auto')">AUTO</button>
</div>
</div>
<div class="card">
<h2>💧 Water Tank</h2>
<div class="tankWrapper">
<div class="tank">
<div id="water" class="water"></div>
<div id="waterText" class="waterText">0%</div>
<div class="marker m25">25%</div><div class="marker m50">50%</div><div class="marker m75">75%</div><div class="marker m100">100%</div>
</div>
</div>
</div>
<div class="grid">
<div class="stat"><div class="label">Filtered Distance</div><div class="value"><span id="distance">0</span> cm</div></div>
<div class="stat"><div class="label">Water Level</div><div class="value"><span id="level">0</span> cm</div></div>
<div class="stat"><div class="label">Current Water</div><div class="value"><span id="liters">0</span> L</div></div>
<div class="stat"><div class="label">Tank Capacity</div><div class="value"><span id="capacity">0</span> L</div></div>
<div class="stat"><div class="label">Total Water Used</div><div class="value"><span id="used">0</span> L</div></div>
</div>
<div class="card" style="margin-top:20px;">
<h2>🎚 Motor Threshold</h2>
<div class="grid">
<div class="threshold"><strong>Motor ON</strong><br>Water ≤ <span id="onDisplay">25</span>%<br>20 sec confirmation</div>
<div class="threshold"><strong>Motor OFF</strong><br>Water ≥ <span id="offDisplay">75</span>%<br>10 sec confirmation</div>
</div>
</div>
<div class="card">
<h2>⚙️ Tank & Motor Settings</h2>
<div class="grid">
<div><label>Tank Radius (cm)</label><input id="radius" type="number" step="0.1" oninput="settingsChanged=true"></div>
<div><label>Tank Height (cm)</label><input id="height" type="number" step="0.1" oninput="settingsChanged=true"></div>
<div><label>Motor ON (%)</label><input id="on" type="number" min="0" max="100" step="1" oninput="settingsChanged=true"></div>
<div><label>Motor OFF (%)</label><input id="off" type="number" min="0" max="100" step="1" oninput="settingsChanged=true"></div>
<div><label>Sensor Offset (cm)</label><input id="offset" type="number" step="0.1" oninput="settingsChanged=true"></div>
</div>
<div class="center"><button class="blue" onclick="saveSettings()">💾 SAVE SETTINGS</button></div>
</div>
<div class="card">
<h2>🎯 Sensor Calibration</h2>
<div id="calNotice" class="notice">Keep tank steady before calibration.</div>
<div class="progress"><div id="calBar" class="progressBar"></div></div>
<p class="center"><strong id="calTimer">Ready</strong></p>
<div class="center">
<button class="purple" onclick="calibrate('empty')">CALIBRATE EMPTY</button>
<button class="purple" onclick="calibrate('full')">CALIBRATE FULL</button>
</div>
<div class="grid" style="margin-top:15px;">
<div class="stat"><div class="label">Empty Distance</div><div class="value"><span id="empty">0</span> cm</div></div>
<div class="stat"><div class="label">Full Distance</div><div class="value"><span id="full">0</span> cm</div></div>
<div class="stat"><div class="label">Calibration</div><div class="value" id="calibrated">NO</div></div>
</div>
</div>
<div class="card">
<h2>🔄 Reset</h2>
<div class="center">
<button class="orange" onclick="resetUsage()">RESET WATER USED</button>
<button class="gray" onclick="resetAll()">RESET ALL SETTINGS</button>
</div>
</div>
</div>
<script>
let settingsChanged=false,uiWaterPercent=0,targetWaterPercent=0,uiAnimationStarted=false;
function updateData(){
  fetch('/data').then(r=>r.json()).then(d=>{
    targetWaterPercent=d.percent;
    if(!uiAnimationStarted){uiWaterPercent=targetWaterPercent;uiAnimationStarted=true;}
    document.getElementById('distance').innerText=d.distance.toFixed(2);
    document.getElementById('level').innerText=d.level.toFixed(2);
    document.getElementById('liters').innerText=d.liters.toFixed(2);
    document.getElementById('capacity').innerText=d.capacity.toFixed(2);
    document.getElementById('used').innerText=d.used.toFixed(2);
    let m=document.getElementById('motorStatus');
    m.innerText=d.relay?'ON':'OFF';
    m.className='motorStatus '+(d.relay?'motorOn':'motorOff');
    document.getElementById('motorMode').innerText=d.mode;
    document.getElementById('systemState').innerText=d.state;
    let cd=document.getElementById('countdown');
    if(d.state==='NORMAL') cd.innerText='System stable';
    else if(d.state==='VERIFY_ON') cd.innerText='🔎 Confirming MOTOR ON — '+d.remaining+' seconds remaining';
    else if(d.state==='VERIFY_OFF') cd.innerText='🔎 Confirming MOTOR OFF — '+d.remaining+' seconds remaining';
    else if(d.state==='CALIBRATING') cd.innerText='⏳ Calibration — '+d.remaining+' seconds remaining';
    document.getElementById('onDisplay').innerText=d.on;
    document.getElementById('offDisplay').innerText=d.off;
    document.getElementById('empty').innerText=d.empty.toFixed(2);
    document.getElementById('full').innerText=d.full.toFixed(2);
    document.getElementById('calibrated').innerText=d.calibrated?'YES':'NO';
    if(d.calibrationRunning){
      document.getElementById('calNotice').innerText='⏳ KEEP TANK STEADY — '+d.remaining+' seconds remaining';
      document.getElementById('calBar').style.width=d.progress+'%';
      document.getElementById('calTimer').innerText=d.remaining+' seconds';
    }else{
      document.getElementById('calNotice').innerText='Calibration ready';
      document.getElementById('calBar').style.width='0%';
      document.getElementById('calTimer').innerText='Ready';
    }
    if(!settingsChanged){
      document.getElementById('radius').value=d.radius;
      document.getElementById('height').value=d.height;
      document.getElementById('on').value=d.on;
      document.getElementById('off').value=d.off;
      document.getElementById('offset').value=d.offset;
    }
  }).catch(e=>console.log(e));
}
function animateWater(){
  let diff=targetWaterPercent-uiWaterPercent;
  if(Math.abs(diff)>20) uiWaterPercent+=diff*0.08;
  else if(Math.abs(diff)>10) uiWaterPercent+=diff*0.06;
  else if(Math.abs(diff)>3) uiWaterPercent+=diff*0.04;
  else uiWaterPercent+=diff*0.025;
  if(Math.abs(targetWaterPercent-uiWaterPercent)<0.05) uiWaterPercent=targetWaterPercent;
  if(uiWaterPercent<0) uiWaterPercent=0;
  if(uiWaterPercent>100) uiWaterPercent=100;
  document.getElementById('water').style.height=uiWaterPercent+'%';
  document.getElementById('waterText').innerText=uiWaterPercent.toFixed(1)+'%';
}
function motor(s){fetch('/motor?state='+encodeURIComponent(s)).then(r=>r.text()).then(m=>{alert(m);updateData();});}
function saveSettings(){
  let rad=document.getElementById('radius').value,h=document.getElementById('height').value,o=document.getElementById('on').value,f=document.getElementById('off').value,off=document.getElementById('offset').value;
  if(Number(o)>=Number(f)){alert('Motor OFF percentage must be greater than Motor ON percentage.');return;}
  fetch('/save?radius='+encodeURIComponent(rad)+'&height='+encodeURIComponent(h)+'&on='+encodeURIComponent(o)+'&off='+encodeURIComponent(f)+'&offset='+encodeURIComponent(off)).then(r=>r.text()).then(m=>{alert(m);settingsChanged=false;updateData();});
}
function calibrate(t){if(!confirm('Keep tank steady for 10 seconds. Start '+t+' calibration?'))return;fetch('/calibrate?type='+encodeURIComponent(t)).then(r=>r.text()).then(m=>{alert(m);updateData();});}
function resetUsage(){if(!confirm('Reset total water usage?'))return;fetch('/resetUsage').then(r=>r.text()).then(m=>{alert(m);updateData();});}
function resetAll(){if(!confirm('Reset ALL settings, calibration and water usage?'))return;fetch('/resetAll').then(r=>r.text()).then(m=>{alert(m);settingsChanged=false;uiWaterPercent=0;targetWaterPercent=0;uiAnimationStarted=false;updateData();});}
updateData();setInterval(updateData,1000);setInterval(animateWater,50);
</script>
</body>
</html>
)rawliteral";

/* =====================================================
   SETUP
   ===================================================== */

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);

  // Active LOW relay: LOW = ON, HIGH = OFF
  motorOFF();

  loadSettings();
  loadWiFiSettings();
  calculateTank();

  // AP + STA Mode with Static IP for Hotspot Setup
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);

  // Start Captive DNS Server (Port 53)
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  Serial.println();
  Serial.println("====================================");
  Serial.println("SMART WATER CONTROLLER (MQTT + WEB)");
  Serial.println("====================================");
  Serial.print("ESP32 AP Hotspot IP:   ");
  Serial.println(WiFi.softAPIP());
  Serial.print("ESP32 AP Hotspot SSID: ");
  Serial.println(AP_SSID);

  // Connect to Wi-Fi (Configured -> Primary WIFI/RAHAT -> Fallback)
  attemptWiFiConnection();

  // Setup MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024); // Large buffer for full telemetry JSON

  // Embedded WebServer Routes
  server.on("/", []() {
    server.send_P(200, "text/html", MAIN_PAGE);
  });
  server.on("/data", sendData);
  server.on("/save", saveAPI);
  server.on("/motor", motorAPI);
  server.on("/calibrate", calibrationAPI);
  server.on("/resetUsage", resetUsageAPI);
  server.on("/resetAll", resetAllAPI);

  // WiFi Hotspot Configuration Routes
  server.on("/wifi", handleWifiPage);
  server.on("/wifisave", handleWifiSave);

  // Captive Portal Detection Redirects
  server.on("/generate_204", handleCaptiveRedirect);
  server.on("/gen_204", handleCaptiveRedirect);
  server.on("/hotspot-detect.html", handleCaptiveRedirect);
  server.on("/canonical.html", handleCaptiveRedirect);
  server.on("/ncsi.txt", handleCaptiveRedirect);
  server.on("/connecttest.txt", handleCaptiveRedirect);

  server.begin();

  Serial.println("Local HTTP Server Ready");
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("Calibration: ");
  Serial.println(calibrationReady ? "READY" : "NOT READY");

  lastSample = millis();
  lastNormalCalculation = millis();
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  // Handle Captive Portal DNS queries
  dnsServer.processNextRequest();

  // Handle HTTP client
  server.handleClient();

  // Handle MQTT connection and incoming commands
  handleMQTT();

  unsigned long now = millis();

  // Priority 1: Calibration routine
  if (calibrationRunning) {
    processCalibration();
    // Live countdown update to MQTT every 1s during calibration
    if (now - lastLiveMqttPublish >= 1000) {
      lastLiveMqttPublish = now;
      publishTelemetry();
    }
    return;
  }

  // Priority 2: 0.5s Sensor Sampling
  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    sampleSensor();
  }

  // Priority 3: 5s Statistical Filtering Window
  if (now - lastNormalCalculation >= NORMAL_WINDOW) {
    lastNormalCalculation = now;
    processNormalData();
  }

  // Priority 4: Confirmation Routine (20s ON / 10s OFF)
  if (confirmationRunning) {
    processConfirmation();
    // Live countdown update to MQTT every 1s during verification
    if (now - lastLiveMqttPublish >= 1000) {
      lastLiveMqttPublish = now;
      publishTelemetry();
    }
  }
}
