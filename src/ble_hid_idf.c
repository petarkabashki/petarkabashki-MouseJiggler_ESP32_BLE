#include "ble_hid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "blehid";

#define REPORTID_MOUSE 1
#define REPORTID_KEYBOARD 2

// The HID descriptor item encodings, same names the Arduino library uses so
// the descriptor below can be read side by side with lib/ESP32-BLE-Mouse.
#define HIDINPUT(size) (0x80 | (size))
#define HIDOUTPUT(size) (0x90 | (size))
#define COLLECTION(size) (0xa0 | (size))
#define END_COLLECTION(size) (0xc0 | (size))
#define USAGE_PAGE(size) (0x04 | (size))
#define LOGICAL_MINIMUM(size) (0x14 | (size))
#define LOGICAL_MAXIMUM(size) (0x24 | (size))
#define REPORT_SIZE(size) (0x74 | (size))
#define REPORT_ID(size) (0x84 | (size))
#define REPORT_COUNT(size) (0x94 | (size))
#define USAGE(size) (0x08 | (size))
#define USAGE_MINIMUM(size) (0x18 | (size))
#define USAGE_MAXIMUM(size) (0x28 | (size))

// Byte-for-byte the Arduino build's descriptor: a five-button mouse with wheel
// and horizontal pan under report 1, a boot-protocol keyboard under report 2.
static const uint8_t hidReportMap[] = {
    USAGE_PAGE(1),      0x01,             // Generic Desktop
    USAGE(1),           0x02,             // Mouse
    COLLECTION(1),      0x01,             // Application
    REPORT_ID(1),       REPORTID_MOUSE,   //
    USAGE(1),           0x01,             //   Pointer
    COLLECTION(1),      0x00,             //   Physical
    USAGE_PAGE(1),      0x09,             //     Button
    USAGE_MINIMUM(1),   0x01,             //     Button 1
    USAGE_MAXIMUM(1),   0x05,             //     Button 5
    LOGICAL_MINIMUM(1), 0x00,             //
    LOGICAL_MAXIMUM(1), 0x01,             //
    REPORT_SIZE(1),     0x01,             //
    REPORT_COUNT(1),    0x05,             //     five button bits
    HIDINPUT(1),        0x02,             //
    REPORT_SIZE(1),     0x03,             //
    REPORT_COUNT(1),    0x01,             //
    HIDINPUT(1),        0x03,             //     three bits of padding
    USAGE_PAGE(1),      0x01,             //     Generic Desktop
    USAGE(1),           0x30,             //     X
    USAGE(1),           0x31,             //     Y
    USAGE(1),           0x38,             //     Wheel
    LOGICAL_MINIMUM(1), 0x81,             //     -127
    LOGICAL_MAXIMUM(1), 0x7f,             //      127
    REPORT_SIZE(1),     0x08,             //
    REPORT_COUNT(1),    0x03,             //
    HIDINPUT(1),        0x06,             //     relative
    USAGE_PAGE(1),      0x0c,             //     Consumer Devices
    USAGE(2),           0x38, 0x02,       //     AC Pan
    LOGICAL_MINIMUM(1), 0x81,             //
    LOGICAL_MAXIMUM(1), 0x7f,             //
    REPORT_SIZE(1),     0x08,             //
    REPORT_COUNT(1),    0x01,             //
    HIDINPUT(1),        0x06,             //
    END_COLLECTION(0),                    //   end Physical
    END_COLLECTION(0),                    // end Application

    USAGE_PAGE(1),      0x01,               // Generic Desktop
    USAGE(1),           0x06,               // Keyboard
    COLLECTION(1),      0x01,               // Application
    REPORT_ID(1),       REPORTID_KEYBOARD,  //
    USAGE_PAGE(1),      0x07,               //   Keyboard/Keypad
    USAGE_MINIMUM(1),   0xe0,               //   Left Control
    USAGE_MAXIMUM(1),   0xe7,               //   Right GUI
    LOGICAL_MINIMUM(1), 0x00,               //
    LOGICAL_MAXIMUM(1), 0x01,               //
    REPORT_SIZE(1),     0x01,               //
    REPORT_COUNT(1),    0x08,               //   modifier byte
    HIDINPUT(1),        0x02,               //
    REPORT_COUNT(1),    0x01,               //
    REPORT_SIZE(1),     0x08,               //
    HIDINPUT(1),        0x03,               //   reserved byte
    REPORT_COUNT(1),    0x05,               //
    REPORT_SIZE(1),     0x01,               //
    USAGE_PAGE(1),      0x08,               //   LEDs
    USAGE_MINIMUM(1),   0x01,               //   Num Lock
    USAGE_MAXIMUM(1),   0x05,               //   Kana
    HIDOUTPUT(1),       0x02,               //
    REPORT_COUNT(1),    0x01,               //
    REPORT_SIZE(1),     0x03,               //
    HIDOUTPUT(1),       0x03,               //   LED padding
    REPORT_COUNT(1),    0x06,               //
    REPORT_SIZE(1),     0x08,               //
    LOGICAL_MINIMUM(1), 0x00,               //
    LOGICAL_MAXIMUM(1), 0x65,               //
    USAGE_PAGE(1),      0x07,               //   Keyboard/Keypad
    USAGE_MINIMUM(1),   0x00,               //
    USAGE_MAXIMUM(1),   0x65,               //
    HIDINPUT(1),        0x00,               //   six simultaneous keys
    END_COLLECTION(0),                      // end Application
};

static esp_hid_raw_report_map_t reportMaps[] = {
    {.data = hidReportMap, .len = sizeof(hidReportMap)},
};

static esp_hid_device_config_t hidConfig = {
    // Logitech's IDs, as the Arduino build uses: some hosts treat an unknown
    // vendor's HID mouse with suspicion, and this one is always allowed.
    .vendor_id = 0x046D,
    .product_id = 0xB023,
    .version = 0x0100,
    .device_name = "ESP32 Mouse",
    .manufacturer_name = "Espressif",
    .serial_number = "0001",
    .report_maps = reportMaps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t *hidDev = NULL;
static volatile bool connected = false;
static bool advertising = false;
// Captured from GAP, because the HID layer's connect event does not carry it
// and `ble-drop` needs an address to hang up on.
static esp_bd_addr_t peerAddr;
static bool peerKnown = false;

static esp_ble_adv_params_t advParams = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

void bleHidAdvStart(void) {
  if (esp_ble_gap_start_advertising(&advParams) == ESP_OK) advertising = true;
}

void bleHidAdvStop(void) {
  esp_ble_gap_stop_advertising();
  advertising = false;
}

static void gapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
      bleHidAdvStart();
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      // Nothing on this device can display or confirm a passkey, so accepting
      // is the only answer that lets a pairing finish.
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      memcpy(peerAddr, param->ble_security.auth_cmpl.bd_addr, sizeof(peerAddr));
      peerKnown = param->ble_security.auth_cmpl.success;
      ESP_LOGI(TAG, "pairing %s",
               param->ble_security.auth_cmpl.success ? "succeeded" : "failed");
      break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      memcpy(peerAddr, param->update_conn_params.bda, sizeof(peerAddr));
      peerKnown = true;
      break;
    default:
      break;
  }
}

static void hidHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)base;
  switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
      bleHidAdvStart();
      break;
    case ESP_HIDD_CONNECT_EVENT:
      connected = true;
      advertising = false;
      break;
    case ESP_HIDD_DISCONNECT_EVENT:
      connected = false;
      // Without this the device goes quiet after the host walks away, which
      // for a jiggler is the one failure nobody notices until it matters.
      bleHidAdvStart();
      break;
    default:
      break;
  }
}

static bool gapConfigure(const char *deviceName) {
  // 0x1812 (HID) as a 128-bit UUID, which is what the adv payload wants.
  static const uint8_t hidServiceUuid[] = {
      0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
      0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
  };
  esp_ble_adv_data_t advData = {
      .set_scan_rsp = false,
      .include_name = true,
      .include_txpower = true,
      .min_interval = 0x0006,
      .max_interval = 0x0010,
      .appearance = ESP_HID_APPEARANCE_MOUSE,
      .manufacturer_len = 0,
      .p_manufacturer_data = NULL,
      .service_data_len = 0,
      .p_service_data = NULL,
      .service_uuid_len = sizeof(hidServiceUuid),
      .p_service_uuid = (uint8_t *)hidServiceUuid,
      .flag = 0x06,
  };

  esp_ble_auth_req_t authReq = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t ioCap = ESP_IO_CAP_NONE; // no display, no keypad
  uint8_t initKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rspKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t keySize = 16;

  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authReq, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ioCap, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initKey, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rspKey, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, 1);

  if (esp_ble_gap_set_device_name(deviceName) != ESP_OK) return false;
  // Advertising starts from ADV_DATA_SET_COMPLETE, not here: the payload has
  // to be in place before the first packet goes out.
  return esp_ble_gap_config_adv_data(&advData) == ESP_OK;
}

bool bleHidInit(const char *deviceName) {
  if (hidDev) return true;
  if (deviceName && *deviceName) hidConfig.device_name = deviceName;

  esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (esp_bt_controller_init(&btCfg) != ESP_OK) return false;
  if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return false;
  if (esp_bluedroid_init() != ESP_OK) return false;
  if (esp_bluedroid_enable() != ESP_OK) return false;

  if (esp_ble_gap_register_callback(gapHandler) != ESP_OK) return false;
  if (!gapConfigure(hidConfig.device_name)) return false;
  if (esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler) != ESP_OK) return false;

  if (esp_hidd_dev_init(&hidConfig, ESP_HID_TRANSPORT_BLE, hidHandler, &hidDev) != ESP_OK) {
    hidDev = NULL;
    return false;
  }
  esp_hidd_dev_battery_set(hidDev, 100);
  return true;
}

bool bleHidConnected(void) { return connected && hidDev && esp_hidd_dev_connected(hidDev); }

void bleHidMouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  if (!bleHidConnected()) return;
  uint8_t report[5] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel, (uint8_t)pan};
  esp_hidd_dev_input_set(hidDev, 0, REPORTID_MOUSE, report, sizeof(report));
}

void bleHidKeyboard(uint8_t modifiers, const uint8_t keys[6]) {
  if (!bleHidConnected()) return;
  uint8_t report[8] = {modifiers, 0};
  if (keys) memcpy(report + 2, keys, 6);
  esp_hidd_dev_input_set(hidDev, 0, REPORTID_KEYBOARD, report, sizeof(report));
}

const char *bleHidMac(void) {
  static char buf[18] = "";
  const uint8_t *a = esp_bt_dev_get_address();
  if (!a) return "(radio is off)";
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

void bleHidDisconnect(void) {
  // Tearing the HID service down would also end advertising; hanging up on the
  // link leaves the device discoverable and the host free to come back.
  if (peerKnown) esp_ble_gap_disconnect(peerAddr);
}

int bleHidBondCount(void) {
  int n = esp_ble_get_bond_device_num();
  return n < 0 ? 0 : n;
}

void bleHidBondsClear(void) {
  int n = bleHidBondCount();
  if (n <= 0) return;
  esp_ble_bond_dev_t *list = calloc((size_t)n, sizeof(esp_ble_bond_dev_t));
  if (!list) return;
  if (esp_ble_get_bond_device_list(&n, list) == ESP_OK)
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
}
