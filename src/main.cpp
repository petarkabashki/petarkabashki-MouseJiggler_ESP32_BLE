#include <Arduino.h>
#include <BleMouse.h>
#include <esp_task_wdt.h>
#include <Preferences.h> // Library for saving state to memory
#include <WiFi.h>
#include <BLEDevice.h>
#include <esp_bt_device.h>
#include <esp_gap_ble_api.h>

#include "forth.h"       // Embedded command language on the serial terminal
#include "forth_ffi.h"   // Calling compiled code by address, from Forth
#include "forth_files.h" // ls/cat/include/edit/… over POSIX

#ifdef HAS_OLED
#include <U8g2lib.h>
// ESP32-C3 0.42" OLED "egg" board: SSD1306-compatible 72x40 panel on I2C (SDA=5, SCL=6)
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, /* SCL */ 6, /* SDA */ 5);
#endif

// --- Main Configuration ---
// Esp32 wroom
// #define BUTTON_PIN 0  // GPIO0 is commonly available and safe for button input on ESP32-WROOM
// #define LED_PIN 2  // GPIO2 is the onboard LED on most ESP32-WROOM modules

// Esp32-C3 SuperMini
// --- Main Configuration ---
// Update the button and LED pins as per your ESP32-C3 SuperMini schematic
#define BUTTON_PIN 9
#define LED_PIN 8 

#define BOOT_FILE "/boot.fs"   // always run at startup, unless BOOT is held

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

// Live, tunable copies of the jiggle parameters. The #defines above are only
// the power-on defaults; these are bound into Forth as variables, so they can
// be changed from the terminal and persisted with `save`.
struct JigglerConfig {
  int32_t slowInterval, slowRange;
  int32_t fastInterval, fastRange;
  int32_t zigzagInterval, zigzagRange, zigzagPhase;
  int32_t keepAlive;
};

static const JigglerConfig defaultConfig = {
  SLOW_INTERVAL, SLOW_RANGE,
  FAST_INTERVAL, FAST_RANGE,
  ZIGZAG_INTERVAL, ZIGZAG_RANGE, ZIGZAG_PHASE_LENGTH,
  30000
};
static JigglerConfig cfg = defaultConfig;

// Optional caption shown on the display's bottom line, set with `status!`.
static char statusLine[20] = "";

// --- State Variables ---
unsigned long lastButtonPressTime = 0;
int buttonPressCount = 0;
bool buttonLastState = HIGH;
bool wasConnected = false;
// Exposed to Forth as `zig-step`; the scribble's drift direction is derived
// from it. Jiggle timing state lives in Forth variables (see jiggler_forth.h).
int32_t zigzagStep = 0;
#ifdef HAS_OLED
JiggleMode lastOledMode = NONE;
bool lastOledConnected = false;
void updateOLED(bool connected); // defined below setup()
#endif

static void registerForthWords(); // defined below, next to the word bodies
static void runBootScript();      // the Forth jiggler
static void fw_loadWords();
static void wdtYield();
static void loadConfig();
static void saveConfig();

void setup() {
  Serial.begin(115200);

  preferences.begin("jiggler-prefs", false);
  jiggleMode = (JiggleMode)preferences.getUChar("lastMode", NONE);
  loadConfig();

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

#ifdef HAS_OLED
  u8g2.begin();
  u8g2.setPowerSave(0);
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);
  updateOLED(false); // Force an initial draw — the loop() only redraws on change
  lastOledMode = jiggleMode;
  lastOledConnected = false;
#endif

  esp_task_wdt_init(3, true);
  esp_task_wdt_add(NULL);

  forthIO = &Serial;
  forthYield = wdtYield;
  forthInit();
  forthFilesInit();   // mounts the flash filesystem and adds the file words
  registerForthWords();
  forthLineSink = forthFilesSink;
  runBootScript();   // the jiggler, written in Forth
  forthMarkCore();   // everything after this point is the user's own

  // Hold BOOT while powering up to come back bare: no boot.fs, no saved
  // words. The way out of a boot script that bricks the prompt.
  bool safeMode = (digitalRead(BUTTON_PIN) == LOW);

  if (safeMode) {
    Serial.println("🔒 SAFE MODE — boot.fs and saved words skipped.");
  } else {
    fw_loadWords();  // definitions saved with `save-words`
    if (forthFilesExists(BOOT_FILE)) {
      Serial.printf("running %s\r\n", BOOT_FILE);
      forthFilesInclude(BOOT_FILE);
    }
  }

  Serial.println("Forth terminal ready — type `help` or `words`.");
  Serial.print("ok> ");
}

const char *modeName(JiggleMode mode) {
  switch (mode) {
    case SLOW:   return "SLOW";
    case FAST:   return "FAST";
    case ZIGZAG: return "ZIGZAG";
    case NONE:
    default:     return "OFF";
  }
}

#ifdef HAS_OLED
// When false, the built-in status layout stops redrawing and Forth scripts own
// the display (see the `screen-auto` word).
static bool oledAuto = true;

// 🖥️ Redraws the OLED with the current mode and BLE connection state
void updateOLED(bool connected) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "BLE Jiggler");
  u8g2.drawHLine(0, 13, 72);

  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(2, 30, modeName(jiggleMode));

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 39, statusLine[0] ? statusLine : (connected ? "Connected" : "No link"));

  u8g2.sendBuffer();
}
#endif

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

// ---------------------------------------------------------------------------
// The jiggle engine itself now lives in Forth (src/jiggler_forth.h). What stays
// here is only persistence.
// ---------------------------------------------------------------------------
static void saveConfig() {
  preferences.putUChar("lastMode", (uint8_t)jiggleMode);
  preferences.putBytes("cfg", &cfg, sizeof(cfg));
}

static void loadConfig() {
  if (preferences.getBytesLength("cfg") == sizeof(cfg))
    preferences.getBytes("cfg", &cfg, sizeof(cfg));
}

// ---------------------------------------------------------------------------
// Forth bindings — the whole "bridge" is one small function per exposed call.
// ---------------------------------------------------------------------------
static cell forthTaskXt = -1;
static unsigned long forthTaskInterval = 1000;
static unsigned long forthTaskLast = 0;

static void wdtYield() { esp_task_wdt_reset(); }

static signed char clamp127(cell v) {
  if (v > 127) return 127;
  if (v < -127) return -127;
  return (signed char)v;
}

static void fw_move()    { cell dy = forthPop(), dx = forthPop(); bleMouse.move(clamp127(dx), clamp127(dy)); }
static void fw_scroll()  { bleMouse.move(0, 0, clamp127(forthPop())); }
static void fw_click()   { bleMouse.click((uint8_t)forthPop()); }
static void fw_press()   { bleMouse.press((uint8_t)forthPop()); }
static void fw_release() { bleMouse.release((uint8_t)forthPop()); }
static void fw_conn()    { forthPush(bleMouse.isConnected() ? -1 : 0); }

static void fw_modeFetch() { forthPush((cell)jiggleMode); }

static void fw_modeStore() {
  cell m = forthPop();
  if (m < NONE || m > ZIGZAG) { forthAbort("mode must be 0..3"); return; }
  jiggleMode = (JiggleMode)m;
  if (jiggleMode == ZIGZAG) zigzagStep = 0;
}

static void fw_modeName() { forthPushString(modeName(jiggleMode)); }

static void fw_save()     { saveConfig(); }
static void fw_restore()  { loadConfig(); }
static void fw_defaults() { cfg = defaultConfig; }

// ( addr len -- )  caption for the display's bottom line; empty string clears it
static void fw_statusStore() {
  forthPopString(statusLine, sizeof(statusLine));
#ifdef HAS_OLED
  lastOledMode = (JiggleMode)-1; // force the next loop() pass to redraw
#endif
}

static void fw_state() {
  Serial.printf("mode %s  connected %s  uptime %lus  heap %u\r\n",
                modeName(jiggleMode), bleMouse.isConnected() ? "yes" : "no",
                millis() / 1000, (unsigned)ESP.getFreeHeap());
  Serial.printf("slow %ldms/%ldpx  fast %ldms/%ldpx  zigzag %ldms/%ldpx phase %ld  keepalive %ldms\r\n",
                (long)cfg.slowInterval, (long)cfg.slowRange,
                (long)cfg.fastInterval, (long)cfg.fastRange,
                (long)cfg.zigzagInterval, (long)cfg.zigzagRange, (long)cfg.zigzagPhase,
                (long)cfg.keepAlive);
  if (statusLine[0]) Serial.printf("status \"%s\"\r\n", statusLine);
}

// ---------------------------------------------------------------------------
// Display words. Registered unconditionally; they are no-ops on boards without
// a panel, so the same scripts run everywhere.
// ---------------------------------------------------------------------------
static void fw_hasScreen() {
#ifdef HAS_OLED
  forthPush(-1);
#else
  forthPush(0);
#endif
}

static void fw_cls() {
#ifdef HAS_OLED
  u8g2.clearBuffer();
#endif
}

static void fw_show() {
#ifdef HAS_OLED
  u8g2.sendBuffer();
#endif
}

// ( addr len x y -- )  draw text with its baseline at y
static void fw_puts() {
  cell y = forthPop(), x = forthPop();
  char text[32];
  if (!forthPopString(text, sizeof(text))) return;
#ifdef HAS_OLED
  u8g2.drawStr((int)x, (int)y, text);
#else
  (void)x; (void)y;
#endif
}

// ( n x y -- )  draw a number, saving a string buffer for the common case
static void fw_putn() {
  cell y = forthPop(), x = forthPop(), n = forthPop();
  char text[16];
  snprintf(text, sizeof(text), "%ld", (long)n);
#ifdef HAS_OLED
  u8g2.drawStr((int)x, (int)y, text);
#else
  (void)x; (void)y;
#endif
}

// ( n -- )  0 = tiny 4x6, 1 = normal 6x10, 2 = bold 7x14
static void fw_font() {
  cell n = forthPop();
#ifdef HAS_OLED
  switch (n) {
    case 0:  u8g2.setFont(u8g2_font_4x6_tf);   break;
    case 2:  u8g2.setFont(u8g2_font_7x14B_tf); break;
    default: u8g2.setFont(u8g2_font_6x10_tf);  break;
  }
#else
  (void)n;
#endif
}

static void fw_hline() { // ( x y w -- )
  cell w = forthPop(), y = forthPop(), x = forthPop();
#ifdef HAS_OLED
  u8g2.drawHLine((int)x, (int)y, (int)w);
#else
  (void)w; (void)y; (void)x;
#endif
}

static void fw_box() { // ( x y w h -- ) filled
  cell h = forthPop(), w = forthPop(), y = forthPop(), x = forthPop();
#ifdef HAS_OLED
  u8g2.drawBox((int)x, (int)y, (int)w, (int)h);
#else
  (void)h; (void)w; (void)y; (void)x;
#endif
}

static void fw_frame() { // ( x y w h -- ) outline
  cell h = forthPop(), w = forthPop(), y = forthPop(), x = forthPop();
#ifdef HAS_OLED
  u8g2.drawFrame((int)x, (int)y, (int)w, (int)h);
#else
  (void)h; (void)w; (void)y; (void)x;
#endif
}

static void fw_pixel() { // ( x y -- )
  cell y = forthPop(), x = forthPop();
#ifdef HAS_OLED
  u8g2.drawPixel((int)x, (int)y);
#else
  (void)y; (void)x;
#endif
}

static void fw_contrast() {
  cell n = forthPop();
#ifdef HAS_OLED
  u8g2.setContrast((uint8_t)n);
#else
  (void)n;
#endif
}

// ( flag -- )  false hands the display over to scripts
static void fw_screenAuto() {
  cell f = forthPop();
#ifdef HAS_OLED
  oledAuto = (f != 0);
  if (oledAuto) lastOledMode = (JiggleMode)-1; // redraw the status layout now
#else
  (void)f;
#endif
}

static void fw_ms() {
  cell n = forthPop();
  unsigned long start = millis();
  while ((long)(millis() - start) < (long)n) {
    esp_task_wdt_reset();
    delay(1);
  }
}

static void fw_millis()  { forthPush((cell)millis()); }
static void fw_led()     { digitalWrite(LED_PIN, forthPop() ? LOW : HIGH); } // active-low
static void fw_pinWrite(){ cell pin = forthPop(), v = forthPop(); digitalWrite((uint8_t)pin, v ? HIGH : LOW); }
static void fw_pinRead() { forthPush(digitalRead((uint8_t)forthPop()) ? -1 : 0); }
static void fw_pinMode() { cell pin = forthPop(), m = forthPop(); pinMode((uint8_t)pin, (uint8_t)m); }
static void fw_adc()     { forthPush((cell)analogRead((uint8_t)forthPop())); }
static void fw_heap()    { forthPush((cell)ESP.getFreeHeap()); }
static void fw_reboot()  { ESP.restart(); }
static void fw_random()  { cell hi = forthPop(), lo = forthPop(); forthPush((cell)random(lo, hi)); }

static void fw_taskStore()    { forthTaskXt = forthPop(); forthTaskLast = millis(); }
static void fw_taskOff()      { forthTaskXt = -1; }
static void fw_taskInterval() { forthTaskInterval = (unsigned long)forthPop(); }

// --- keyboard -------------------------------------------------------------
// Modifiers come first so a line reads like the chord: `ctrl k-c key`.
static void fw_key()      { cell u = forthPop(), m = forthPop(); bleMouse.keyTap((uint8_t)u, (uint8_t)m); }
static void fw_keyDown()  { cell u = forthPop(), m = forthPop(); bleMouse.keyPress((uint8_t)u, (uint8_t)m); }
static void fw_keyUp()    { bleMouse.keyRelease((uint8_t)forthPop()); }
static void fw_keyClear() { bleMouse.releaseAll(); }
static void fw_keyChar()  { bleMouse.writeChar((char)forthPop()); }
static void fw_keys() { // ( addr len -- ) type a string
  char buf[128];
  if (!forthPopString(buf, sizeof(buf))) return;
  bleMouse.writeString(buf);
}

// --- BLE ------------------------------------------------------------------
static void fw_bleMac() {
  const uint8_t *m = esp_bt_dev_get_address();
  char buf[20] = "(none)";
  if (m) snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  forthPushString(buf);
}
static void fw_bleName()  { forthPushString(bleMouse.deviceName.c_str()); }
static void fw_bleAdv()   { BLEDevice::startAdvertising(); }
static void fw_bleStop()  { BLEDevice::getAdvertising()->stop(); }
static void fw_bleDrop() { // hang up on the current host; advertising resumes
  if (bleMouse.server && bleMouse.server->getConnectedCount())
    bleMouse.server->disconnect(bleMouse.server->getConnId());
}
static void fw_bleBonds() {
  int n = esp_ble_get_bond_device_num();
  esp_ble_bond_dev_t list[8];
  int want = n > 8 ? 8 : n;
  int got = want;
  if (want > 0) esp_ble_get_bond_device_list((int *)&got, list);
  for (int i = 0; i < got; i++) {
    const uint8_t *a = list[i].bd_addr;
    Serial.printf("  %02x:%02x:%02x:%02x:%02x:%02x\r\n", a[0], a[1], a[2], a[3], a[4], a[5]);
  }
  Serial.printf("  %d bonded device(s)\r\n", n);
}
static void fw_bleUnbond() { // forget every paired host, so they must re-pair
  int n = esp_ble_get_bond_device_num();
  esp_ble_bond_dev_t list[8];
  int got = n > 8 ? 8 : n;
  if (got > 0) esp_ble_get_bond_device_list((int *)&got, list);
  for (int i = 0; i < got; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  Serial.printf("removed %d bond(s)\r\n", got);
}

// --- WiFi -----------------------------------------------------------------
// Credentials are held here and, with `wifi-save`, in NVS — as plaintext.
static char wifiSsid[33] = "";
static char wifiPass[65] = "";

static void fw_ssidStore() { forthPopString(wifiSsid, sizeof(wifiSsid)); }
static void fw_passStore() { forthPopString(wifiPass, sizeof(wifiPass)); }
static void fw_wifiSave() {
  preferences.putString("ssid", wifiSsid);
  preferences.putString("pass", wifiPass);
}
static void fw_wifiConnect() {
  if (!wifiSsid[0]) { forthAbort("no ssid — use  s\" name\" ssid!"); return; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  Serial.printf("connecting to %s ...\r\n", wifiSsid);
}
static void fw_wifiConn()  { forthPush(WiFi.status() == WL_CONNECTED ? -1 : 0); }
static void fw_wifiIp()    { forthPushString(WiFi.localIP().toString().c_str()); }
static void fw_wifiRssi()  { forthPush((cell)WiFi.RSSI()); }
static void fw_wifiMac()   { forthPushString(WiFi.macAddress().c_str()); }
static void fw_wifiOff()   { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
static void fw_wifiScan() {
  // Scanning takes seconds, which is longer than the watchdog's patience, so
  // it runs asynchronously and we feed the WDT while waiting.
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true);
  int n;
  unsigned long deadline = millis() + 12000;
  while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() < deadline) {
    esp_task_wdt_reset();
    delay(50);
  }
  if (n < 0) { Serial.println("scan failed"); WiFi.scanDelete(); return; }
  for (int i = 0; i < n && i < 12; i++)
    Serial.printf("  %-24s %4d dBm %s\r\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "");
  Serial.printf("  %d network(s)\r\n", n);
  WiFi.scanDelete();
}
static void fw_wifiAp() { // ( -- ) access point using the stored credentials
  if (!wifiSsid[0]) { forthAbort("no ssid — use  s\" name\" ssid!"); return; }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(wifiSsid, strlen(wifiPass) >= 8 ? wifiPass : NULL);
  Serial.printf("AP %s at %s\r\n", wifiSsid, WiFi.softAPIP().toString().c_str());
}

// --- files ----------------------------------------------------------------
// The file words live in forth_files.cpp, over POSIX: LittleFS mounts a VFS
// at /littlefs and that is all this build has to contribute, the mount itself
// being in port_arduino.cpp. Writing them this way is what lets the sibling
// project run `ls cat include edit append rm df save-to` unchanged.

// --- persistence of Forth definitions -------------------------------------
static void fw_saveWords() {
  preferences.putString("words", forthSource());
  Serial.printf("saved %d bytes of definitions\r\n", forthSourceLen());
}
static void fw_loadWords() {
  String src = preferences.getString("words", "");
  if (!src.length()) { Serial.println("(nothing saved)"); return; }
  forthLoadSource(src.c_str());
}
static void fw_dropWords() { preferences.remove("words"); forthClearSource(); }

static void fw_help() {
  Serial.println(F(
    "\r\nJiggler Forth. `words` lists everything. Device words:\r\n"
    "  dx dy move        one relative mouse move (-127..127)\r\n"
    "  n scroll          wheel\r\n"
    "  b click/press/release   1=left 2=right 4=middle\r\n"
    "  conn?             -1 if a BLE host is connected\r\n"
    "  mode@ / n mode!   0=off 1=slow 2=fast 3=zigzag;  save  persists it\r\n"
    "  jiggle            one step of the current mode\r\n"
    "  mode$             mode name as a string ( -- addr len )\r\n"
    "  .state            print everything;  save / restore / defaults\r\n"
    "  slow-ms slow-px fast-ms fast-px zig-ms zig-px zig-phase keepalive-ms\r\n"
    "                    live parameters:  2000 fast-ms !\r\n"
    "  n ms  millis  heap  reboot  lo hi random\r\n"
    "  f led  v pin pin!  pin pin@  m pin pinmode  pin adc\r\n"
    "  ' word task!      run `word` periodically;  n every  sets ms;  task-off\r\n"
    "  addr @ / v addr ! read & write any address (peripheral registers included)\r\n"
    "Strings:  s\" text\" ( -- addr len )  type  n string NAME  s! s+ sc+ s# s@ s. slen s=\r\n"
    "Screen:   screen?  cls  show  ( a u x y ) puts  ( n x y ) putn  n font\r\n"
    "          x y w hline  x y w h box|frame  x y pixel  n contrast  f screen-auto\r\n"
    "          s\" text\" status!   caption on the bottom line\r\n"
    "Example:  : wiggle 20 0 do 5 0 move 40 ms -5 0 move 40 ms loop ;\r\n"
    "Keyboard: mods usage key      tap;  key+ hold  key- release  key-clear\r\n"
    "          c keyc   s\" text\" keys   ctrl shift alt gui  k-enter k-tab k-up ...\r\n"
    "BLE:      ble-mac ble-name ble-adv ble-stop ble-drop ble-bonds ble-unbond\r\n"
    "WiFi:     s\" ap\" ssid!  s\" pw\" pass!  wifi-connect  wifi-save  wifi?\r\n"
    "          wifi-ip wifi-rssi wifi-mac wifi-scan wifi-off wifi-ap\r\n"
    "Files:    ls  df  s\" f\" cat|include|rm|edit|append|save-to\r\n"
    "          edit captures typed lines until a line containing just  ;;\r\n"
    "          /boot.fs runs at startup; hold BOOT while booting to skip it\r\n"
    "FFI:      s\" name\" sym ( -- fn )   fn 0call|1call|..|4call ( -- result )\r\n"
    "          args fn n call     s\" x\" cstr ( -- zaddr )   zaddr zcount\r\n"
    "          s\" part\" syms      needs /symbols.txt from `pio run -t uploadfs`\r\n"
    "Vocabulary: user  see NAME  forget NAME  wipe  list\r\n"
    "          save-words / load-words / drop-words   (NVS; replayed at boot)\r\n"
    "Jiggler:  it is Forth — `see tick`, `see jiggle`. Redefine `tick` to\r\n"
    "          change what runs every pass;  forget tick  restores it.\r\n"
    "Example:  false screen-auto  : hud cls 1 font mode$ 2 20 puts show ; hud\r\n"));
}

static void registerForthWords() {
  forthAddWord("move", fw_move);
  forthAddWord("scroll", fw_scroll);
  forthAddWord("click", fw_click);
  forthAddWord("press", fw_press);
  forthAddWord("release", fw_release);
  forthAddWord("conn?", fw_conn);
  forthAddWord("mode@", fw_modeFetch);
  forthAddWord("mode!", fw_modeStore);
  forthAddWord("mode$", fw_modeName);
  forthAddWord("save", fw_save);
  forthAddWord("restore", fw_restore);
  forthAddWord("defaults", fw_defaults);
  forthAddWord("status!", fw_statusStore);
  forthAddWord(".state", fw_state);

  // Display
  forthAddWord("screen?", fw_hasScreen);
  forthAddWord("cls", fw_cls);
  forthAddWord("show", fw_show);
  forthAddWord("puts", fw_puts);
  forthAddWord("putn", fw_putn);
  forthAddWord("font", fw_font);
  forthAddWord("hline", fw_hline);
  forthAddWord("box", fw_box);
  forthAddWord("frame", fw_frame);
  forthAddWord("pixel", fw_pixel);
  forthAddWord("contrast", fw_contrast);
  forthAddWord("screen-auto", fw_screenAuto);

  // Live jiggle parameters — plain C++ variables, reachable with @ and !
  forthAddVariable("slow-ms", &cfg.slowInterval);
  forthAddVariable("slow-px", &cfg.slowRange);
  forthAddVariable("fast-ms", &cfg.fastInterval);
  forthAddVariable("fast-px", &cfg.fastRange);
  forthAddVariable("zig-ms", &cfg.zigzagInterval);
  forthAddVariable("zig-px", &cfg.zigzagRange);
  forthAddVariable("zig-phase", &cfg.zigzagPhase);
  forthAddVariable("keepalive-ms", &cfg.keepAlive);
  forthAddVariable("zig-step", &zigzagStep);
  forthAddWord("ms", fw_ms);
  forthAddWord("millis", fw_millis);
  forthAddWord("led", fw_led);
  forthAddWord("pin!", fw_pinWrite);
  forthAddWord("pin@", fw_pinRead);
  forthAddWord("pinmode", fw_pinMode);
  forthAddWord("adc", fw_adc);
  forthAddWord("heap", fw_heap);
  forthAddWord("reboot", fw_reboot);
  forthAddWord("random", fw_random);
  forthAddWord("task!", fw_taskStore);
  forthAddWord("task-off", fw_taskOff);
  forthAddWord("every", fw_taskInterval);
  forthAddWord("help", fw_help);

  forthAddConstant("off", NONE);
  forthAddConstant("slow", SLOW);
  forthAddConstant("fast", FAST);
  forthAddConstant("zigzag", ZIGZAG);
  forthAddConstant("left", MOUSE_LEFT);
  forthAddConstant("right", MOUSE_RIGHT);
  forthAddConstant("middle", MOUSE_MIDDLE);
  forthAddConstant("led-pin", LED_PIN);
  forthAddConstant("button-pin", BUTTON_PIN);
  forthAddConstant("input", INPUT);
  forthAddConstant("output", OUTPUT);
  forthAddConstant("input-pullup", INPUT_PULLUP);

  // --- keyboard ---
  forthAddWord("key", fw_key);
  forthAddWord("key+", fw_keyDown);
  forthAddWord("key-", fw_keyUp);
  forthAddWord("key-clear", fw_keyClear);
  forthAddWord("keyc", fw_keyChar);
  forthAddWord("keys", fw_keys);
  forthAddConstant("ctrl", KB_CTRL);
  forthAddConstant("shift", KB_SHIFT);
  forthAddConstant("alt", KB_ALT);
  forthAddConstant("gui", KB_GUI);
  forthAddConstant("k-enter", KEY_ENTER);
  forthAddConstant("k-esc", KEY_ESC);
  forthAddConstant("k-bs", KEY_BACKSPACE);
  forthAddConstant("k-tab", KEY_TAB);
  forthAddConstant("k-space", KEY_SPACE);
  forthAddConstant("k-del", KEY_DELETE);
  forthAddConstant("k-home", KEY_HOME);
  forthAddConstant("k-end", KEY_END);
  forthAddConstant("k-pgup", KEY_PAGEUP);
  forthAddConstant("k-pgdn", KEY_PAGEDOWN);
  forthAddConstant("k-up", KEY_UP);
  forthAddConstant("k-down", KEY_DOWN);
  forthAddConstant("k-left", KEY_LEFT);
  forthAddConstant("k-right", KEY_RIGHT);
  forthAddConstant("k-f1", KEY_F1);

  // --- BLE ---
  forthAddWord("ble-mac", fw_bleMac);
  forthAddWord("ble-name", fw_bleName);
  forthAddWord("ble-adv", fw_bleAdv);
  forthAddWord("ble-stop", fw_bleStop);
  forthAddWord("ble-drop", fw_bleDrop);
  forthAddWord("ble-bonds", fw_bleBonds);
  forthAddWord("ble-unbond", fw_bleUnbond);

  // --- WiFi ---
  forthAddWord("ssid!", fw_ssidStore);
  forthAddWord("pass!", fw_passStore);
  forthAddWord("wifi-connect", fw_wifiConnect);
  forthAddWord("wifi-save", fw_wifiSave);
  forthAddWord("wifi?", fw_wifiConn);
  forthAddWord("wifi-ip", fw_wifiIp);
  forthAddWord("wifi-rssi", fw_wifiRssi);
  forthAddWord("wifi-mac", fw_wifiMac);
  forthAddWord("wifi-scan", fw_wifiScan);
  forthAddWord("wifi-off", fw_wifiOff);
  forthAddWord("wifi-ap", fw_wifiAp);

  // --- files ---
  // --- FFI: reach compiled code that nobody wrote a word for ---
  // esp_restart is in every image and never inlined, so its address is a cheap
  // fingerprint of the build the symbol table was generated from.
  forthFilesBindSymbols("esp_restart", (cell)(void *)&esp_restart);
  ffiRegisterWords();

  // --- persistence of user definitions ---
  forthAddWord("save-words", fw_saveWords);
  forthAddWord("load-words", fw_loadWords);
  forthAddWord("drop-words", fw_dropWords);
}

#include "jiggler_forth.h"


static cell tickXt = -1;

static void runBootScript() {
  for (unsigned i = 0; i < sizeof(jigglerForth) / sizeof(jigglerForth[0]); i++)
    forthEval(jigglerForth[i]);
  tickXt = forthFind("tick");
}

// Runs the user's periodic word, if one was installed with `task!`.
void runForthTask() {
  if (forthTaskXt < 0) return;
  if (millis() - forthTaskLast < forthTaskInterval) return;
  forthTaskLast = millis();
  forthExecuteXt(forthTaskXt);
}

void loop() {
  esp_task_wdt_reset();
  handleButtonPress();
  updateLED();

  bool connected = bleMouse.isConnected();

#ifdef HAS_OLED
  if (oledAuto && (jiggleMode != lastOledMode || connected != lastOledConnected)) {
    updateOLED(connected);
    lastOledMode = jiggleMode;
    lastOledConnected = connected;
  }
#endif

  if (connected && !wasConnected) {
    Serial.println("✅ BLE Connected");
    wasConnected = true;
  } else if (!connected && wasConnected) {
    Serial.println("❌ BLE Disconnected — advertising resumed");
    wasConnected = false;
  }
  
  // Jiggling and the BLE keep-alive are the Forth word `tick`; redefining it
  // from the terminal changes the device's behaviour on the spot.
  // Resolved every pass rather than cached, so a `: tick ... ;` typed at the
  // prompt takes effect immediately.
  tickXt = forthFind("tick");
  if (tickXt >= 0) forthExecuteXt(tickXt);

  forthPoll();     // terminal command language
  runForthTask();  // user-scripted periodic word, if any

  delay(10);
}