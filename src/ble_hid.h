// BLE HID mouse + keyboard, for the ESP-IDF build.
//
// The Arduino build gets this from the vendored lib/ESP32-BLE-Mouse, which
// builds the GATT service by hand. On IDF the esp_hid component already is a
// HID device: this file supplies the report map and the pairing parameters and
// otherwise stays out of the way, which is why it is a few hundred lines rather
// than a few thousand.
//
// The report map, the report IDs and the Logitech VID/PID are the same as the
// Arduino build's, so a host that has paired with one sees the same device.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the controller, Bluedroid, GAP and the HID service, then starts
// advertising. Returns false if any of that fails.
bool bleHidInit(const char *deviceName);

bool bleHidConnected(void);

// ( buttons, dx, dy, wheel, pan ) — one relative mouse report.
void bleHidMouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan);

// Modifier bitmap plus up to six simultaneous usages, as the boot protocol
// wants them. Pass all zeroes to release everything.
void bleHidKeyboard(uint8_t modifiers, const uint8_t keys[6]);

void bleHidAdvStart(void);
void bleHidAdvStop(void);

// The device's own address, as "aa:bb:cc:dd:ee:ff". Points at a static buffer.
const char *bleHidMac(void);

// Drops the current link; the host usually reconnects on its own.
void bleHidDisconnect(void);

// Bonded hosts: count them, or forget all of them so pairing starts over.
int bleHidBondCount(void);
void bleHidBondsClear(void);

#ifdef __cplusplus
}
#endif
