#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "BLE2902.h"
#include "BLEHIDDevice.h"
#include "HIDTypes.h"
#include "HIDKeyboardTypes.h"
#include <driver/adc.h>
#include "sdkconfig.h"

#include "BleConnectionStatus.h"
#include "BleMouse.h"

#if defined(CONFIG_ARDUHAL_ESP_LOG)
  #include "esp32-hal-log.h"
  #define LOG_TAG ""
#else
  #include "esp_log.h"
  static const char* LOG_TAG = "BLEDevice";
#endif

#define REPORTID_MOUSE    1
#define REPORTID_KEYBOARD 2

static const uint8_t _hidReportDescriptor[] = {
  USAGE_PAGE(1),       0x01, // USAGE_PAGE (Generic Desktop)
  USAGE(1),            0x02, // USAGE (Mouse)
  COLLECTION(1),       0x01, // COLLECTION (Application)
  REPORT_ID(1),        REPORTID_MOUSE,
  USAGE(1),            0x01, //   USAGE (Pointer)
  COLLECTION(1),       0x00, //   COLLECTION (Physical)
  // ------------------------------------------------- Buttons (Left, Right, Middle, Back, Forward)
  USAGE_PAGE(1),       0x09, //     USAGE_PAGE (Button)
  USAGE_MINIMUM(1),    0x01, //     USAGE_MINIMUM (Button 1)
  USAGE_MAXIMUM(1),    0x05, //     USAGE_MAXIMUM (Button 5)
  LOGICAL_MINIMUM(1),  0x00, //     LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1),  0x01, //     LOGICAL_MAXIMUM (1)
  REPORT_SIZE(1),      0x01, //     REPORT_SIZE (1)
  REPORT_COUNT(1),     0x05, //     REPORT_COUNT (5)
  HIDINPUT(1),         0x02, //     INPUT (Data, Variable, Absolute) ;5 button bits
  // ------------------------------------------------- Padding
  REPORT_SIZE(1),      0x03, //     REPORT_SIZE (3)
  REPORT_COUNT(1),     0x01, //     REPORT_COUNT (1)
  HIDINPUT(1),         0x03, //     INPUT (Constant, Variable, Absolute) ;3 bit padding
  // ------------------------------------------------- X/Y position, Wheel
  USAGE_PAGE(1),       0x01, //     USAGE_PAGE (Generic Desktop)
  USAGE(1),            0x30, //     USAGE (X)
  USAGE(1),            0x31, //     USAGE (Y)
  USAGE(1),            0x38, //     USAGE (Wheel)
  LOGICAL_MINIMUM(1),  0x81, //     LOGICAL_MINIMUM (-127)
  LOGICAL_MAXIMUM(1),  0x7f, //     LOGICAL_MAXIMUM (127)
  REPORT_SIZE(1),      0x08, //     REPORT_SIZE (8)
  REPORT_COUNT(1),     0x03, //     REPORT_COUNT (3)
  HIDINPUT(1),         0x06, //     INPUT (Data, Variable, Relative) ;3 bytes (X,Y,Wheel)
  // ------------------------------------------------- Horizontal wheel
  USAGE_PAGE(1),       0x0c, //     USAGE PAGE (Consumer Devices)
  USAGE(2),      0x38, 0x02, //     USAGE (AC Pan)
  LOGICAL_MINIMUM(1),  0x81, //     LOGICAL_MINIMUM (-127)
  LOGICAL_MAXIMUM(1),  0x7f, //     LOGICAL_MAXIMUM (127)
  REPORT_SIZE(1),      0x08, //     REPORT_SIZE (8)
  REPORT_COUNT(1),     0x01, //     REPORT_COUNT (1)
  HIDINPUT(1),         0x06, //     INPUT (Data, Var, Rel)
  END_COLLECTION(0),         //   END_COLLECTION
  END_COLLECTION(0),         // END_COLLECTION

  // ----------------------------------------------------------- Keyboard
  USAGE_PAGE(1),       0x01, // USAGE_PAGE (Generic Desktop)
  USAGE(1),            0x06, // USAGE (Keyboard)
  COLLECTION(1),       0x01, // COLLECTION (Application)
  REPORT_ID(1),        REPORTID_KEYBOARD,
  // ------------------------------------------------- Modifier byte
  USAGE_PAGE(1),       0x07, //   USAGE_PAGE (Keyboard/Keypad)
  USAGE_MINIMUM(1),    0xe0, //   USAGE_MINIMUM (Left Control)
  USAGE_MAXIMUM(1),    0xe7, //   USAGE_MAXIMUM (Right GUI)
  LOGICAL_MINIMUM(1),  0x00, //   LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1),  0x01, //   LOGICAL_MAXIMUM (1)
  REPORT_SIZE(1),      0x01, //   REPORT_SIZE (1)
  REPORT_COUNT(1),     0x08, //   REPORT_COUNT (8)
  HIDINPUT(1),         0x02, //   INPUT (Data, Variable, Absolute)
  // ------------------------------------------------- Reserved byte
  REPORT_COUNT(1),     0x01, //   REPORT_COUNT (1)
  REPORT_SIZE(1),      0x08, //   REPORT_SIZE (8)
  HIDINPUT(1),         0x03, //   INPUT (Constant)
  // ------------------------------------------------- LED output report
  REPORT_COUNT(1),     0x05, //   REPORT_COUNT (5)
  REPORT_SIZE(1),      0x01, //   REPORT_SIZE (1)
  USAGE_PAGE(1),       0x08, //   USAGE_PAGE (LEDs)
  USAGE_MINIMUM(1),    0x01, //   USAGE_MINIMUM (Num Lock)
  USAGE_MAXIMUM(1),    0x05, //   USAGE_MAXIMUM (Kana)
  HIDOUTPUT(1),        0x02, //   OUTPUT (Data, Variable, Absolute)
  REPORT_COUNT(1),     0x01, //   REPORT_COUNT (1)
  REPORT_SIZE(1),      0x03, //   REPORT_SIZE (3)
  HIDOUTPUT(1),        0x03, //   OUTPUT (Constant) ; LED padding
  // ------------------------------------------------- Six simultaneous keys
  REPORT_COUNT(1),     0x06, //   REPORT_COUNT (6)
  REPORT_SIZE(1),      0x08, //   REPORT_SIZE (8)
  LOGICAL_MINIMUM(1),  0x00, //   LOGICAL_MINIMUM (0)
  LOGICAL_MAXIMUM(1),  0x65, //   LOGICAL_MAXIMUM (101)
  USAGE_PAGE(1),       0x07, //   USAGE_PAGE (Keyboard/Keypad)
  USAGE_MINIMUM(1),    0x00, //   USAGE_MINIMUM (0)
  USAGE_MAXIMUM(1),    0x65, //   USAGE_MAXIMUM (101)
  HIDINPUT(1),         0x00, //   INPUT (Data, Array)
  END_COLLECTION(0)          // END_COLLECTION
};

BleMouse::BleMouse(std::string deviceName, std::string deviceManufacturer, uint8_t batteryLevel) : 
    _buttons(0),
    _modifiers(0),
    hid(0),
    inputKeyboard(0),
    server(0)
{
  memset(this->_keys, 0, sizeof(this->_keys));
  this->deviceName = deviceName;
  this->deviceManufacturer = deviceManufacturer;
  this->batteryLevel = batteryLevel;
  this->connectionStatus = new BleConnectionStatus();
}

void BleMouse::begin(void)
{
  xTaskCreate(this->taskServer, "server", 20000, (void *)this, 5, NULL);
}

void BleMouse::end(void)
{
}

void BleMouse::click(uint8_t b)
{
  _buttons = b;
  move(0,0,0,0);
  _buttons = 0;
  move(0,0,0,0);
}

void BleMouse::move(signed char x, signed char y, signed char wheel, signed char hWheel)
{
  if (this->isConnected())
  {
    uint8_t m[5];
    m[0] = _buttons;
    m[1] = x;
    m[2] = y;
    m[3] = wheel;
    m[4] = hWheel;
    this->inputMouse->setValue(m, 5);
    this->inputMouse->notify();
  }
}

void BleMouse::buttons(uint8_t b)
{
  if (b != _buttons)
  {
    _buttons = b;
    move(0,0,0,0);
  }
}

void BleMouse::press(uint8_t b)
{
  buttons(_buttons | b);
}

void BleMouse::release(uint8_t b)
{
  buttons(_buttons & ~b);
}

bool BleMouse::isPressed(uint8_t b)
{
  if ((b & _buttons) > 0)
    return true;
  return false;
}

// --------------------------------------------------------------------------
// Keyboard
// --------------------------------------------------------------------------
void BleMouse::sendKeyReport(void)
{
  if (!this->isConnected() || this->inputKeyboard == nullptr) return;
  uint8_t k[8];
  k[0] = _modifiers;
  k[1] = 0; // reserved
  memcpy(&k[2], _keys, 6);
  this->inputKeyboard->setValue(k, 8);
  this->inputKeyboard->notify();
}

void BleMouse::keyPress(uint8_t usage, uint8_t modifiers)
{
  _modifiers |= modifiers;
  if (usage) {
    for (int i = 0; i < 6; i++) if (_keys[i] == usage) { sendKeyReport(); return; }
    for (int i = 0; i < 6; i++) if (_keys[i] == 0) { _keys[i] = usage; break; }
  }
  sendKeyReport();
}

void BleMouse::keyRelease(uint8_t usage)
{
  for (int i = 0; i < 6; i++) if (_keys[i] == usage) _keys[i] = 0;
  sendKeyReport();
}

void BleMouse::releaseAll(void)
{
  _modifiers = 0;
  memset(_keys, 0, sizeof(_keys));
  sendKeyReport();
}

void BleMouse::keyTap(uint8_t usage, uint8_t modifiers)
{
  keyPress(usage, modifiers);
  delay(8); // hosts drop keystrokes that arrive as a single same-millisecond report pair
  releaseAll();
  delay(8);
}

void BleMouse::writeChar(char c)
{
  uint8_t idx = (uint8_t)c;
  if (idx >= KEYMAP_SIZE) return;
  KEYMAP km = keymap[idx];
  if (km.usage == 0) return;
  keyTap(km.usage, km.modifier);
}

void BleMouse::writeString(const char* text)
{
  if (!text) return;
  while (*text) writeChar(*text++);
}

bool BleMouse::isConnected(void) {
  return this->connectionStatus->connected;
}

void BleMouse::setBatteryLevel(uint8_t level) {
  this->batteryLevel = level;
  if (hid != 0)
      this->hid->setBatteryLevel(this->batteryLevel);
}

void BleMouse::taskServer(void* pvParameter) {
  BleMouse* bleMouseInstance = (BleMouse *) pvParameter; //static_cast<BleMouse *>(pvParameter);
  BLEDevice::init(bleMouseInstance->deviceName);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(bleMouseInstance->connectionStatus);
  bleMouseInstance->server = pServer;

  bleMouseInstance->hid = new BLEHIDDevice(pServer);
  bleMouseInstance->inputMouse = bleMouseInstance->hid->inputReport(REPORTID_MOUSE);
  bleMouseInstance->inputKeyboard = bleMouseInstance->hid->inputReport(REPORTID_KEYBOARD);
  bleMouseInstance->hid->outputReport(REPORTID_KEYBOARD); // keyboard LED state, unused
  bleMouseInstance->connectionStatus->inputMouse = bleMouseInstance->inputMouse;

  bleMouseInstance->hid->manufacturer()->setValue(bleMouseInstance->deviceManufacturer);

  // bleMouseInstance->hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
  // BleMouse bleMouse("MX Master 3", "Logitech, Inc.", 100);
  // Vendor ID source = USB (0x02), Vendor = 0x046D, Product = 0xB023, Version = 0x0100
  bleMouseInstance->hid->pnp(0x02, 0x046D, 0xB023, 0x0100);

  bleMouseInstance->hid->hidInfo(0x00,0x02);

  BLESecurity *pSecurity = new BLESecurity();

  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  // Without these, no LTK/IRK is actually exchanged during bonding, so
  // BlueZ later fails to read the encrypted HID characteristics with
  // "Request attribute has encountered an unlikely error" even though
  // the pairing itself appeared to succeed.
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  bleMouseInstance->hid->reportMap((uint8_t*)_hidReportDescriptor, sizeof(_hidReportDescriptor));
  bleMouseInstance->hid->startServices();

  bleMouseInstance->onStarted(pServer);

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->setAppearance(HID_MOUSE);
  pAdvertising->addServiceUUID(bleMouseInstance->hid->hidService()->getUUID());
  
  // Little-endian: 0x6D,0x04 = 0x046D (Logitech), then 0x23,0xB0 = 0xB023 (MX Master 3)
  std::string mfr = "";
  mfr.push_back(0x6D);
  mfr.push_back(0x04);
  mfr.push_back(0x23);
  mfr.push_back(0xB0);
  BLEAdvertisementData advertisementData;
  advertisementData.setManufacturerData(mfr);
  pAdvertising->setAdvertisementData(advertisementData);

  pAdvertising->start();
  bleMouseInstance->hid->setBatteryLevel(bleMouseInstance->batteryLevel);

  ESP_LOGD(LOG_TAG, "Advertising started!");
  vTaskDelay(portMAX_DELAY); //delay(portMAX_DELAY);
}
