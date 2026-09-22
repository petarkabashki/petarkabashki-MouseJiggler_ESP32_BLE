#ifndef ESP32_BLE_MOUSE_H
#define ESP32_BLE_MOUSE_H
#include "sdkconfig.h"
#if defined(CONFIG_BT_ENABLED)

#include "BleConnectionStatus.h"
#include "BLEHIDDevice.h"
#include "BLECharacteristic.h"
#include "BLEServer.h"

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4
#define MOUSE_BACK 8
#define MOUSE_FORWARD 16
#define MOUSE_ALL (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE) # For compatibility with the Mouse library

// Keyboard modifier bits (HID standard order)
#define KB_CTRL      0x01
#define KB_SHIFT     0x02
#define KB_ALT       0x04
#define KB_GUI       0x08
#define KB_RIGHTCTRL 0x10
#define KB_RIGHTSHIFT 0x20
#define KB_RIGHTALT  0x40
#define KB_RIGHTGUI  0x80

// Common non-printable HID usage codes, for keyTap()
#define KEY_ENTER     0x28
#define KEY_ESC       0x29
#define KEY_BACKSPACE 0x2a
#define KEY_TAB       0x2b
#define KEY_SPACE     0x2c
#define KEY_CAPSLOCK  0x39
#define KEY_F1        0x3a
#define KEY_HOME      0x4a
#define KEY_PAGEUP    0x4b
#define KEY_DELETE    0x4c
#define KEY_END       0x4d
#define KEY_PAGEDOWN  0x4e
#define KEY_RIGHT     0x4f
#define KEY_LEFT      0x50
#define KEY_DOWN      0x51
#define KEY_UP        0x52

// Despite the name, this is now a combo HID device: it reports as a mouse
// (report ID 1) and a keyboard (report ID 2) over a single BLE connection.
class BleMouse {
private:
  uint8_t _buttons;
  uint8_t _modifiers;
  uint8_t _keys[6];
  BleConnectionStatus* connectionStatus;
  BLEHIDDevice* hid;
  BLECharacteristic* inputMouse;
  BLECharacteristic* inputKeyboard;
  void buttons(uint8_t b);
  void sendKeyReport(void);
  void rawAction(uint8_t msg[], char msgSize);
  static void taskServer(void* pvParameter);
public:
  // --- keyboard ---
  // Holds a key down (up to six at once) and releases it again.
  void keyPress(uint8_t usage, uint8_t modifiers = 0);
  void keyRelease(uint8_t usage);
  void releaseAll(void);
  void keyTap(uint8_t usage, uint8_t modifiers = 0);
  // Types printable ASCII, applying shift where the character needs it.
  void writeChar(char c);
  void writeString(const char* text);

  BleMouse(std::string deviceName = "ESP32 Bluetooth Mouse", std::string deviceManufacturer = "Espressif", uint8_t batteryLevel = 100);
  void begin(void);
  void end(void);
  void click(uint8_t b = MOUSE_LEFT);
  void move(signed char x, signed char y, signed char wheel = 0, signed char hWheel = 0);
  void press(uint8_t b = MOUSE_LEFT);   // press LEFT by default
  void release(uint8_t b = MOUSE_LEFT); // release LEFT by default
  bool isPressed(uint8_t b = MOUSE_LEFT); // check LEFT by default
  bool isConnected(void);
  void setBatteryLevel(uint8_t level);
  uint8_t batteryLevel;
  BLEServer* server;  // set once the server task is up; null before that
  std::string deviceManufacturer;
  std::string deviceName;
protected:
  virtual void onStarted(BLEServer *pServer) { };
};

#endif // CONFIG_BT_ENABLED
#endif // ESP32_BLE_MOUSE_H
