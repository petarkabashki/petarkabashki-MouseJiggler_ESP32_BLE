#include "BleConnectionStatus.h"

BleConnectionStatus::BleConnectionStatus(void) {
}

void BleConnectionStatus::onConnect(BLEServer* pServer)
{
  this->connected = true;
  BLE2902* desc = (BLE2902*)this->inputMouse->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
  desc->setNotifications(true);
}

void BleConnectionStatus::onDisconnect(BLEServer* pServer)
{
  this->connected = false;
  BLE2902* desc = (BLE2902*)this->inputMouse->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
  desc->setNotifications(false);
  // BLEServer does not auto-resume advertising after a disconnect, so kick it
  // back on here instead of tearing down/reinitializing the whole BLE stack
  // (which corrupted the bonded encryption context on every reconnect).
  pServer->startAdvertising();
}
