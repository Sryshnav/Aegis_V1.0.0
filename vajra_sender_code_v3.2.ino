#include "driver/rtc_io.h" // NEW: Required to hold pins during sleep

// --- Pin Definitions (XIAO ESP32-S3 Sense) ---
const int IGNITION_PIN = D3; 
const int POT_PIN = A0;
const int JOY_X_PIN = A1;   
const int JOY_Y_PIN = A2;
const int LED_ON_PIN = D4;
const int LED_OFF_PIN = D5;

// --- PHASE 5: RTC (Real-Time Clock) MEMORY ---
RTC_DATA_ATTR int ignitionState = 0;      
RTC_DATA_ATTR bool isImmobilized = false;
RTC_DATA_ATTR int sleepIntervalSec = 60; 

int lastIgnitionButtonState = 1;
int lastSpeed = -99, lastSteering = -99, lastIgnition = -99;
float lastBatteryVolt = -99.0;

// --- ANTI-FLOOD RATE LIMITING ---
unsigned long lastSendTime = 0;
unsigned long lastChangeTime = 0;
int updateRateMs = 1000; 

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, D7, D6);
  pinMode(IGNITION_PIN, INPUT_PULLUP);
  pinMode(LED_ON_PIN, OUTPUT);
  pinMode(LED_OFF_PIN, OUTPUT);
  delay(1000); 

  // --- PHASE 5: WAKEUP REASON CHECK ---
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    ignitionState = 1; 
    lastIgnitionButtonState = 0; // NEW: Ignores your finger so it doesn't instantly turn off!
    Serial.println("\n☀️ SENDER WOKE UP: IGNITION TURNED ON!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("\n💓 SENDER WOKE UP: HEARTBEAT PING...");
  } else {
    Serial.println("\n--- SENDER (BIKE NODE) INITIALIZED ---");
  }
}

void loop() {
  // 1. LISTEN FOR REMOTE COMMANDS
  if (Serial1.available() > 0) {
    String cmd = Serial1.readStringUntil('\n');
    cmd.trim();
    if (cmd == "LOCK") {
      isImmobilized = true;
      Serial.println("🛑 VEHICLE REMOTE LOCKED");
    } else if (cmd == "UNLOCK") {
      isImmobilized = false;
      Serial.println("🟢 VEHICLE REMOTE RELEASED");
    } else if (cmd.startsWith("RATE:")) {
      updateRateMs = cmd.substring(5).toInt();
    } else if (cmd.startsWith("HEARTBEAT:")) {
      sleepIntervalSec = cmd.substring(10).toInt(); 
      Serial.println("⏱️ Heartbeat updated to: " + String(sleepIntervalSec) + "s");
    }
  }

  // 2. READ SENSORS
  float currentBatteryVolt = 10.0 + (analogRead(POT_PIN) / 4095.0) * 5.0;
  int currentIgnitionReading = digitalRead(IGNITION_PIN);
  
  if (lastIgnitionButtonState == 1 && currentIgnitionReading == 0) {
    ignitionState = !ignitionState; 
    delay(50);
  }
  lastIgnitionButtonState = currentIgnitionReading;

  int currentSpeed = 0, currentSteering = 0;
  if (ignitionState == 1 && !isImmobilized) {
    int yVal = analogRead(JOY_Y_PIN);
    currentSpeed = (yVal > 2200) ? map(yVal, 2200, 4095, 0, 120) : 0; 
    int xVal = analogRead(JOY_X_PIN);
    if (xVal < 1900) currentSteering = map(xVal, 1900, 0, 0, -90);
    else if (xVal > 2200) currentSteering = map(xVal, 2200, 4095, 0, 90);
  }

  // 3. LED LOGIC 
  if (isImmobilized) {
    digitalWrite(LED_ON_PIN, LOW);
    digitalWrite(LED_OFF_PIN, HIGH);
  } else {
    digitalWrite(LED_ON_PIN, ignitionState ? HIGH : LOW);
    digitalWrite(LED_OFF_PIN, ignitionState ? LOW : HIGH);
  }

  // 4. TRANSMIT DATA 
  bool stateChanged = (abs(currentSpeed - lastSpeed) > 5 || abs(currentSteering - lastSteering) > 5 || 
      abs(currentBatteryVolt - lastBatteryVolt) > 0.3 || ignitionState != lastIgnition);

  if ((stateChanged && (millis() - lastChangeTime >= 300)) || (millis() - lastSendTime >= updateRateMs) || ignitionState == 0) {
    String packet = String(ignitionState) + "," + String(currentBatteryVolt, 1) + "," + 
                    String(currentSpeed) + "," + String(currentSteering);
    Serial.print("Sending: "); Serial.println(packet);
    Serial1.println(packet); 

    lastIgnition = ignitionState; lastSpeed = currentSpeed; 
    lastSteering = currentSteering; lastBatteryVolt = currentBatteryVolt;
    lastChangeTime = millis();
    lastSendTime = millis();
  }

  // --- PHASE 5: THE DEEP SLEEP TRIGGER ---
  if (ignitionState == 0) {
    Serial.println("⏳ Waiting 1 second for incoming cloud commands...");
    
    unsigned long waitStart = millis();
    while (millis() - waitStart < 1000) {
      if (Serial1.available() > 0) {
        String cmd = Serial1.readStringUntil('\n');
        cmd.trim();
        if (cmd == "LOCK") isImmobilized = true;
        else if (cmd == "UNLOCK") isImmobilized = false;
        else if (cmd.startsWith("HEARTBEAT:")) sleepIntervalSec = cmd.substring(10).toInt();
      }
    }

    // NEW: Wait for the user to physically let go of the button before sleeping!
    if (digitalRead(IGNITION_PIN) == LOW) {
      Serial.println("✋ Please release the Ignition Button...");
      while (digitalRead(IGNITION_PIN) == LOW) { delay(10); }
      delay(100); 
    }

    Serial.println("💤 Entering Deep Sleep for " + String(sleepIntervalSec) + " seconds...");
    
    // NEW: Force the internal pull-up resistor to stay ON during sleep
    rtc_gpio_pullup_en((gpio_num_t)IGNITION_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)IGNITION_PIN);
    
    esp_sleep_enable_ext0_wakeup((gpio_num_t)IGNITION_PIN, 0); 
    esp_sleep_enable_timer_wakeup(sleepIntervalSec * 1000000ULL); 
    
    esp_deep_sleep_start(); 
  }

  delay(10); 
}