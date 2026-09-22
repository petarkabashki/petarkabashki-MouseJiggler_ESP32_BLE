// port.h on Arduino-ESP32. Thin by design: every one of these is a one-liner
// over the Arduino core, which is exactly why the core is replaceable.
#include "port.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include <stdio.h>

#include "forth.h" // for forthIO, which stays the Arduino build's redirect hook

static Stream *io() { return forthIO ? forthIO : (Stream *)&Serial; }

void portConsoleInit(void) { Serial.begin(115200); }

void portPut(const char *s) { io()->print(s); }
void portPutc(char c) { io()->write((uint8_t)c); }
void portFlush(void) { io()->flush(); }

void portPrintf(const char *fmt, ...) {
  // Stream::printf exists on ESP32 but not on every Arduino core, and going
  // through vsnprintf keeps the backend honest about the buffer size.
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  io()->print(buf);
}

int portGetc(void) {
  Stream *s = io();
  return s->available() ? s->read() : -1;
}

uint32_t portMillis(void) { return (uint32_t)millis(); }

void portDelay(uint32_t ms) {
  // delay() already yields to the scheduler; the watchdog reset is for the
  // case where a Forth word sits in a long `ms` with nothing else running.
  uint32_t end = (uint32_t)millis() + ms;
  while ((int32_t)(end - (uint32_t)millis()) > 0) {
    delay(1);
    portYield();
  }
}

void portYield(void) { esp_task_wdt_reset(); }

// LittleFS registers a VFS at this prefix, so the portable file words reach it
// through fopen() and never touch the Arduino File class.
#define FS_MOUNT "/littlefs"

static bool fsMounted;

bool portFsMountInit(void) {
  fsMounted = LittleFS.begin(true); // formats the empty partition on first run
  return fsMounted;
}

const char *portFsMount(void) { return FS_MOUNT; }

bool portFsInfo(uint32_t *used, uint32_t *total) {
  if (!fsMounted) return false;
  *used = (uint32_t)LittleFS.usedBytes();
  *total = (uint32_t)LittleFS.totalBytes();
  return true;
}

uint32_t portFreeHeap(void) { return (uint32_t)ESP.getFreeHeap(); }
void portRestart(void) { ESP.restart(); }

int32_t portRandom(int32_t lo, int32_t hi) {
  if (hi <= lo) return lo;
  return (int32_t)random(lo, hi);
}
