#include <Arduino.h>
#include <BleMouse.h>
#include <esp_task_wdt.h>
#include <Preferences.h> // Library for saving state to memory

// --- Main Configuration ---
// Esp32 wroom
// #define BUTTON_PIN 0  // GPIO0 is commonly available and safe for button input on ESP32-WROOM
// #define LED_PIN 2  // GPIO2 is the onboard LED on most ESP32-WROOM modules

// Esp32-C3 SuperMini
// --- Main Configuration ---
// Update the button and LED pins as per your ESP32-C3 SuperMini schematic
#define BUTTON_PIN 9
#define LED_PIN 8 

#define DOUBLE_CLICK_TIME 500 // Time in ms to detect a double/triple click

// --- Jiggle Parameters (Customize these values) ---
#define SLOW_INTERVAL 60000   // 60 seconds
#define SLOW_RANGE 50         // Moves between -50 and +50 pixels
#define FAST_INTERVAL 1000    // 1 second
#define FAST_RANGE 3          // Moves between -3 and +3 pixels
#define ZIGZAG_INTERVAL 50    // Faster interval for smoother scribbling
#define ZIGZAG_RANGE 15       // Max distance for each small jittery movement
#define ZIGZAG_PHASE_LENGTH 30 // How many steps before changing drift direction

// --- BLE Mouse Setup ---
BleMouse bleMouse("MX Master 3", "Logitech, Inc.", 100);

// BleMouse bleMouse("Wireless Jiggler", "ACME Corp", 100);
Preferences preferences; // Object to handle saving data

// --- Jiggle Modes ---
enum JiggleMode { NONE, SLOW, FAST, ZIGZAG };
JiggleMode jiggleMode = NONE;

// --- State Variables ---
unsigned long lastButtonPressTime = 0;
unsigned long lastJiggleTime = 0;
unsigned long lastKeepAliveTime = 0;
int buttonPressCount = 0;
bool buttonLastState = HIGH;
bool wasConnected = false;
int zigzagStep = 0;

void setup() {
  Serial.begin(115200);

  preferences.begin("jiggler-prefs", false);
  jiggleMode = (JiggleMode)preferences.getUChar("lastMode", NONE);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  // For an Active-LOW LED, HIGH is the OFF state.
  digitalWrite(LED_PIN, HIGH); 

  bleMouse.begin();
  Serial.println("🔌 BLE Mouse Jiggler Started");

  if (jiggleMode != NONE) {
    Serial.println("✅ Restored last used jiggle mode.");
  } else {
    Serial.println("Press the button to change modes.");
  }

  esp_task_wdt_init(3, true);
  esp_task_wdt_add(NULL);
}

// 💡 Manages the LED to show the current mode
void updateLED() {
  switch (jiggleMode) {
    case SLOW:
      // Use ! to invert the blink signal for an Active-LOW LED
      digitalWrite(LED_PIN, !( (millis() / 1000) % 2 ));
      break;
    case FAST:
      digitalWrite(LED_PIN, !( (millis() / 250) % 2 ));
      break;
    case ZIGZAG:
      digitalWrite(LED_PIN, !( (millis() / 100) % 2 ));
      break;
    case NONE:
    default:
      // For an Active-LOW LED, HIGH turns it OFF.
      digitalWrite(LED_PIN, HIGH);
      break;
  }
}

// 👆 Handles presses and saves the new mode to memory
void handleButtonPress() {
  bool buttonState = digitalRead(BUTTON_PIN);

  if (buttonLastState == HIGH && buttonState == LOW) {
    unsigned long now = millis();
    if (now - lastButtonPressTime < DOUBLE_CLICK_TIME) {
      buttonPressCount++;
    } else {
      buttonPressCount = 1;
    }
    lastButtonPressTime = now;
  }
  buttonLastState = buttonState;

  if (millis() - lastButtonPressTime > DOUBLE_CLICK_TIME && buttonPressCount > 0) {
    JiggleMode newMode = jiggleMode;

    if (buttonPressCount == 1) {
      newMode = (jiggleMode != NONE) ? NONE : SLOW;
    } else if (buttonPressCount == 2) {
      newMode = FAST;
    } else if (buttonPressCount == 3) {
      newMode = ZIGZAG;
    }

    if (newMode != jiggleMode) {
        jiggleMode = newMode;
        preferences.putUChar("lastMode", jiggleMode);

        if (jiggleMode == NONE) Serial.println("🛑 Jiggle mode: OFF");
        if (jiggleMode == SLOW) Serial.println("🟢 Jiggle mode: SLOW");
        if (jiggleMode == FAST) Serial.println("⚡ Jiggle mode: FAST");
        if (jiggleMode == ZIGZAG) {
            zigzagStep = 0;
            Serial.println("🌀 Jiggle mode: ZIGZAG (Natural Scribble)");
        }
    }
    
    buttonPressCount = 0;
  }
}

void tryReconnectBLE() {
  bleMouse.end();
  delay(500);
  bleMouse.begin();
  Serial.println("🔁 BLE service restarted");
}

void loop() {
  esp_task_wdt_reset();
  handleButtonPress();
  updateLED();

  bool connected = bleMouse.isConnected();

  if (connected && !wasConnected) {
    Serial.println("✅ BLE Connected");
    wasConnected = true;
  } else if (!connected && wasConnected) {
    Serial.println("❌ BLE Disconnected — attempting recovery...");
    wasConnected = false;
    tryReconnectBLE();
    return;
  }
  
  if (connected && millis() - lastKeepAliveTime > 30000) {
    bleMouse.move(0, 0);
    lastKeepAliveTime = millis();
  }

  if (connected && jiggleMode != NONE) {
    unsigned long now = millis();
    unsigned long interval = 0;

    switch (jiggleMode) {
      case SLOW:   interval = SLOW_INTERVAL; break;
      case FAST:   interval = FAST_INTERVAL;  break;
      case ZIGZAG: interval = ZIGZAG_INTERVAL;   break;
      default:     return;
    }

    if (now - lastJiggleTime >= interval) {
      int dx = 0, dy = 0;
      if (jiggleMode == SLOW) {
        dx = random(-SLOW_RANGE, SLOW_RANGE + 1);
        dy = random(-SLOW_RANGE, SLOW_RANGE + 1);
      } else if (jiggleMode == FAST) {
        dx = random(-FAST_RANGE, FAST_RANGE + 1);
        dy = random(-FAST_RANGE, FAST_RANGE + 1);
      } else if (jiggleMode == ZIGZAG) {
        int directionBias = (zigzagStep / ZIGZAG_PHASE_LENGTH) % 4;
        switch (directionBias) {
          case 0: // Bias Down-Right
            dx = random(0, ZIGZAG_RANGE + 1);
            dy = random(0, ZIGZAG_RANGE + 1);
            break;
          case 1: // Bias Down-Left
            dx = random(-ZIGZAG_RANGE, 1);
            dy = random(0, ZIGZAG_RANGE + 1);
            break;
          case 2: // Bias Up-Left
            dx = random(-ZIGZAG_RANGE, 1);
            dy = random(-ZIGZAG_RANGE, 1);
            break;
          case 3: // Bias Up-Right
            dx = random(0, ZIGZAG_RANGE + 1);
            dy = random(-ZIGZAG_RANGE, 1);
            break;
        }
        zigzagStep++;
      }
      
      bleMouse.move(dx, dy);
      lastJiggleTime = now;
    }
  }

  delay(10);
}