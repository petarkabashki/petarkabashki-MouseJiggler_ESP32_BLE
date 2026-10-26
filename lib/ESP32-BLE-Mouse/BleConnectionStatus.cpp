#include "BleConnectionStatus.h"
#include <esp_gap_ble_api.h>

BleConnectionStatus::BleConnectionStatus(void) {
}

void BleConnectionStatus::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param)
{
  this->connected = true;
  // Ask for encryption straight away: pairing a new host, or re-encrypting
  // with the stored LTK for a bonded one. Left to the host, BlueZ reads the
  // HID Report Reference descriptors first, gets "insufficient
  // authentication", and can end up with a HID device that drops every report.
  esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
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
