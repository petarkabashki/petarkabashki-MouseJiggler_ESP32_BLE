// Entry point for the ESP-IDF build: Forth on the bare framework, no Arduino.
//
// This is the same interpreter the Arduino build runs (src/forth.cpp, unchanged
// between them) sitting on src/port_idf.c instead of src/port_arduino.cpp, and
// the same jiggler (src/jiggler_forth.h) over src/ble_hid_idf.c instead of the
// vendored Arduino BLE library.
//
// Still Arduino-only, and still in src/main.cpp: the OLED, WiFi, the filesystem
// words and NVS persistence of user definitions.
#include "ble_hid.h"
#include "forth.h"
#include "forth_ffi.h"
#include "forth_files.h"
#include "jiggler_forth.h"
#include "port.h"

#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <string.h>

#ifndef LED_PIN
#define LED_PIN 8
#endif
#ifndef BUTTON_PIN
#define BUTTON_PIN 9
#endif

#define DEVICE_NAME "ESP32 Mouse"

#define BOOT_FILE "boot.fs" // always run at startup, unless BOOT is held

// --- jiggler state, exposed to Forth as variables --------------------------
// Same names and defaults as the Arduino build, so a boot.fs written for one
// works on the other.
static int32_t jiggleMode = 1; // 0 off, 1 slow, 2 fast, 3 zigzag
static int32_t slowInterval = 60000, slowPixels = 50;
static int32_t fastInterval = 1000, fastPixels = 3;
static int32_t zigInterval = 50, zigPixels = 15, zigPhase = 30, zigStep = 0;
static int32_t keepaliveInterval = 30000;

// --- words over the port layer ---------------------------------------------

static void fw_ms()     { portDelay((uint32_t)forthPop()); }
static void fw_millis() { forthPush((cell)portMillis()); }
static void fw_heap()   { forthPush((cell)portFreeHeap()); }
static void fw_reboot() { portRestart(); }
static void fw_random() { cell hi = forthPop(), lo = forthPop();
                          forthPush(portRandom((int32_t)lo, (int32_t)hi)); }

// --- BLE mouse and keyboard -------------------------------------------------

static uint8_t buttonState = 0;
static uint8_t keyMods = 0;
static uint8_t keysDown[6];

static int8_t clamp127(cell v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }

static void fw_move() { // ( dx dy -- )
  cell dy = forthPop(), dx = forthPop();
  bleHidMouse(buttonState, clamp127(dx), clamp127(dy), 0, 0);
}
static void fw_scroll() { bleHidMouse(buttonState, 0, 0, clamp127(forthPop()), 0); }
static void fw_press()   { buttonState |= (uint8_t)forthPop(); bleHidMouse(buttonState, 0, 0, 0, 0); }
static void fw_release() { buttonState &= ~(uint8_t)forthPop(); bleHidMouse(buttonState, 0, 0, 0, 0); }
static void fw_click() {
  uint8_t b = (uint8_t)forthPop();
  bleHidMouse((uint8_t)(buttonState | b), 0, 0, 0, 0);
  portDelay(10);
  bleHidMouse(buttonState, 0, 0, 0, 0);
}
static void fw_conn() { forthPush(bleHidConnected() ? -1 : 0); }

static void sendKeys() { bleHidKeyboard(keyMods, keysDown); }

static void fw_keyAdd() { // ( usage -- ) hold
  uint8_t u = (uint8_t)forthPop();
  for (int i = 0; i < 6; i++) if (keysDown[i] == u) { sendKeys(); return; }
  for (int i = 0; i < 6; i++) if (!keysDown[i]) { keysDown[i] = u; break; }
  sendKeys();
}
static void fw_keyDel() { // ( usage -- ) release
  uint8_t u = (uint8_t)forthPop();
  for (int i = 0; i < 6; i++) if (keysDown[i] == u) keysDown[i] = 0;
  sendKeys();
}
static void fw_keyClear() { keyMods = 0; memset(keysDown, 0, sizeof(keysDown)); sendKeys(); }

static void fw_key() { // ( mods usage -- ) tap
  uint8_t u = (uint8_t)forthPop();
  keyMods = (uint8_t)forthPop();
  memset(keysDown, 0, sizeof(keysDown));
  keysDown[0] = u;
  sendKeys();
  portDelay(12);
  fw_keyClear();
}

// --- BLE control ------------------------------------------------------------

static void fw_bleMac()   { forthPushString(bleHidMac()); }
static void fw_bleName()  { forthPushString(DEVICE_NAME); }
static void fw_bleAdv()   { bleHidAdvStart(); }
static void fw_bleStop()  { bleHidAdvStop(); }
static void fw_bleDrop()  { bleHidDisconnect(); }
static void fw_bleBonds() { forthPush(bleHidBondCount()); }
static void fw_bleUnbond(){ bleHidBondsClear(); }

// --- GPIO -------------------------------------------------------------------
// Configured lazily: a pin becomes an output the first time it is written and
// an input the first time it is read, so `5 pin!` works without ceremony.

static uint32_t outputMask = 0, inputMask = 0;

static void ensureOutput(int pin) {
  if (outputMask & (1u << pin)) return;
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_OUTPUT;
  gpio_config(&c);
  outputMask |= 1u << pin;
  inputMask &= ~(1u << pin);
}

static void ensureInput(int pin) {
  if (inputMask & (1u << pin)) return;
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  inputMask |= 1u << pin;
  outputMask &= ~(1u << pin);
}

static void fw_pinSet() { // ( v pin -- )
  int pin = (int)forthPop();
  cell v = forthPop();
  if (pin < 0 || pin > 21) { forthAbort("pin out of range"); return; }
  ensureOutput(pin);
  gpio_set_level((gpio_num_t)pin, v ? 1 : 0);
}

static void fw_pinGet() { // ( pin -- v )
  int pin = (int)forthPop();
  if (pin < 0 || pin > 21) { forthAbort("pin out of range"); return; }
  ensureInput(pin);
  forthPush(gpio_get_level((gpio_num_t)pin));
}

// The devkit LED is active-low, matching the Arduino build's `led`.
static void fw_led() {
  cell on = forthPop();
  ensureOutput(LED_PIN);
  gpio_set_level((gpio_num_t)LED_PIN, on ? 0 : 1);
}

// --- mode -------------------------------------------------------------------

static const char *modeName(int32_t m) {
  switch (m) {
    case 0: return "OFF";
    case 1: return "SLOW";
    case 2: return "FAST";
    case 3: return "ZIGZAG";
    default: return "?";
  }
}
static void fw_modeAt()   { forthPush(jiggleMode); }
static void fw_modeStore(){ cell m = forthPop(); jiggleMode = (m < 0 || m > 3) ? 0 : (int32_t)m; }
static void fw_modeName() { forthPushString(modeName(jiggleMode)); }

static void fw_state() {
  portPrintf("  mode      %s\r\n", modeName(jiggleMode));
  portPrintf("  connected %s\r\n", bleHidConnected() ? "yes" : "no");
  portPrintf("  address   %s\r\n", bleHidMac());
  portPrintf("  slow      %ld ms / %ld px\r\n", (long)slowInterval, (long)slowPixels);
  portPrintf("  fast      %ld ms / %ld px\r\n", (long)fastInterval, (long)fastPixels);
  portPrintf("  zigzag    %ld ms / %ld px / phase %ld\r\n",
             (long)zigInterval, (long)zigPixels, (long)zigPhase);
  portPrintf("  keepalive %ld ms\r\n", (long)keepaliveInterval);
  portPrintf("  heap      %lu bytes free\r\n", (unsigned long)portFreeHeap());
}

static void fw_help() {
  portPut(
    "\r\nJiggler Forth on ESP-IDF (no Arduino). `words` lists everything.\r\n"
    "  dx dy move   n scroll   b click/press/release   conn?\r\n"
    "  mode@ / n mode!   0=off 1=slow 2=fast 3=zigzag    mode$   .state\r\n"
    "  jiggle       one step of the current mode\r\n"
    "  slow-ms slow-px fast-ms fast-px zig-ms zig-px zig-phase keepalive-ms\r\n"
    "  mods usage key   usage key+ / key-   key-clear\r\n"
    "  ble-mac ble-name ble-adv ble-stop ble-drop ble-bonds ble-unbond\r\n"
    "  n ms  millis  heap  reboot  lo hi random\r\n"
    "  f led  v pin pin!  pin pin@   addr @ / v addr !\r\n"
    "Files: ls  s\" f\" cat/include/edit/append/rm/save-to   df\r\n"
    "  save-words load-words drop-words    boot.fs runs at startup\r\n"
    "FFI: s\" name\" sym  fn 0call..4call  args fn n call  cstr  zcount\r\n"
    "The jiggler is Forth: `see tick`, `see jiggle`. Redefine `tick` to change\r\n"
    "what runs every pass;  forget tick  restores it.\r\n");
}

// --- persistence of Forth definitions --------------------------------------
// The Arduino build uses Preferences for this; underneath, Preferences is NVS,
// and the key and namespace here are the same ones, so a device flashed from
// either build reads back the other's saved words.
#define NVS_NAMESPACE "jiggler-prefs"
#define NVS_WORDS_KEY "words"

static nvs_handle_t prefs;

static void fw_saveWords() {
  if (nvs_set_str(prefs, NVS_WORDS_KEY, forthSource()) != ESP_OK ||
      nvs_commit(prefs) != ESP_OK) {
    forthAbort("could not save");
    return;
  }
  portPrintf("saved %d bytes of definitions\r\n", forthSourceLen());
}

static void fw_loadWords() {
  // The source log is 3 KB, so a static buffer is the right size by definition
  // and keeps the promise that the interpreter never calls malloc.
  static char buf[3200];
  size_t n = sizeof(buf);
  if (nvs_get_str(prefs, NVS_WORDS_KEY, buf, &n) != ESP_OK || !buf[0]) {
    portPut("(nothing saved)\r\n");
    return;
  }
  forthLoadSource(buf);
}

static void fw_dropWords() {
  nvs_erase_key(prefs, NVS_WORDS_KEY);
  nvs_commit(prefs);
  forthClearSource();
}

static void registerWords() {
  forthAddWord("move", fw_move);
  forthAddWord("scroll", fw_scroll);
  forthAddWord("click", fw_click);
  forthAddWord("press", fw_press);
  forthAddWord("release", fw_release);
  forthAddWord("conn?", fw_conn);

  forthAddWord("key", fw_key);
  forthAddWord("key+", fw_keyAdd);
  forthAddWord("key-", fw_keyDel);
  forthAddWord("key-clear", fw_keyClear);
  forthAddConstant("ctrl", 0x01);
  forthAddConstant("shift", 0x02);
  forthAddConstant("alt", 0x04);
  forthAddConstant("gui", 0x08);
  forthAddConstant("k-enter", 0x28);
  forthAddConstant("k-esc", 0x29);
  forthAddConstant("k-bs", 0x2a);
  forthAddConstant("k-tab", 0x2b);
  forthAddConstant("k-space", 0x2c);
  forthAddConstant("k-right", 0x4f);
  forthAddConstant("k-left", 0x50);
  forthAddConstant("k-down", 0x51);
  forthAddConstant("k-up", 0x52);

  forthAddWord("ble-mac", fw_bleMac);
  forthAddWord("ble-name", fw_bleName);
  forthAddWord("ble-adv", fw_bleAdv);
  forthAddWord("ble-stop", fw_bleStop);
  forthAddWord("ble-drop", fw_bleDrop);
  forthAddWord("ble-bonds", fw_bleBonds);
  forthAddWord("ble-unbond", fw_bleUnbond);

  forthAddWord("mode@", fw_modeAt);
  forthAddWord("mode!", fw_modeStore);
  forthAddWord("mode$", fw_modeName);
  forthAddWord(".state", fw_state);

  forthAddVariable("slow-ms", &slowInterval);
  forthAddVariable("slow-px", &slowPixels);
  forthAddVariable("fast-ms", &fastInterval);
  forthAddVariable("fast-px", &fastPixels);
  forthAddVariable("zig-ms", &zigInterval);
  forthAddVariable("zig-px", &zigPixels);
  forthAddVariable("zig-phase", &zigPhase);
  forthAddVariable("zig-step", &zigStep);
  forthAddVariable("keepalive-ms", &keepaliveInterval);

  forthAddWord("ms", fw_ms);
  forthAddWord("millis", fw_millis);
  forthAddWord("heap", fw_heap);
  forthAddWord("reboot", fw_reboot);
  forthAddWord("random", fw_random);
  forthAddWord("pin!", fw_pinSet);
  forthAddWord("pin@", fw_pinGet);
  forthAddWord("led", fw_led);
  forthAddWord("help", fw_help);
  forthAddConstant("led-pin", LED_PIN);
  forthAddConstant("button-pin", BUTTON_PIN);

  forthAddWord("save-words", fw_saveWords);
  forthAddWord("load-words", fw_loadWords);
  forthAddWord("drop-words", fw_dropWords);

  // esp_restart is in every image and never inlined, so its address is a cheap
  // fingerprint of the build the symbol table was generated from.
  forthFilesBindSymbols("esp_restart", (cell)(void *)&esp_restart);
  ffiRegisterWords();
}

extern "C" void app_main(void) {
  portConsoleInit();

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  nvs_open(NVS_NAMESPACE, NVS_READWRITE, &prefs);

  forthYield = portYield;
  forthInit();
  forthFilesInit(); // mounts the flash filesystem and adds the file words
  registerWords();
  forthLineSink = forthFilesSink;
  for (unsigned i = 0; i < sizeof(jigglerForth) / sizeof(jigglerForth[0]); i++)
    forthEval(jigglerForth[i]);
  forthMarkCore(); // everything past here is the user's own

  // Hold BOOT while powering up to come back bare: no boot.fs, no saved words.
  // The way out of a boot script that bricks the prompt.
  gpio_set_direction((gpio_num_t)BUTTON_PIN, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)BUTTON_PIN, GPIO_PULLUP_ONLY);
  if (gpio_get_level((gpio_num_t)BUTTON_PIN) == 0) {
    portPut("SAFE MODE — boot.fs and saved words skipped.\r\n");
  } else {
    fw_loadWords();
    if (forthFilesExists(BOOT_FILE)) {
      portPrintf("running %s\r\n", BOOT_FILE);
      forthFilesInclude(BOOT_FILE);
    }
  }

  if (!bleHidInit(DEVICE_NAME)) portPut("BLE failed to start\r\n");

  portPut("\r\nForth/IDF ready. `help` for the word list.\r\n");
  forthEval("");

  for (;;) {
    forthPoll();
    // Resolved by name every pass rather than cached, so a `: tick ... ;` typed
    // at the prompt takes effect immediately.
    cell tick = forthFind("tick");
    if (tick >= 0) forthExecuteXt(tick);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
