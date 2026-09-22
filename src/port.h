// The platform surface underneath the Forth core.
//
// forth.cpp and forth_ffi.cpp include this and nothing else — no Arduino.h, no
// esp_*.h. Each backend supplies the other side:
//
//   port_arduino.cpp   Arduino-ESP32 (the shipping build)
//   port_idf.c         ESP-IDF, no Arduino  (framework = espidf)
//   port_host.cpp      the desktop test harness
//
// Everything here is either something the interpreter itself needs or a service
// with no portable equivalent. Device functions — the mouse, the screen, WiFi —
// stay where they are; this is the floor, not a hardware abstraction layer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- console -------------------------------------------------------------
// Brings the terminal up. Call before anything else; on Arduino this is
// Serial.begin(), on IDF it installs the USB Serial/JTAG driver.
void portConsoleInit(void);

// The REPL's terminal. Writes are line-buffered by the backend if it wants;
// portFlush() is the only guarantee that characters have left.
void portPut(const char *s);
void portPutc(char c);
void portPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void portFlush(void);

// Returns the next input character, or -1 when none is waiting. Never blocks.
int portGetc(void);

// --- time ----------------------------------------------------------------
uint32_t portMillis(void);
void portDelay(uint32_t ms); // blocking, but feeds the watchdog

// Called from inside long-running Forth words. The default resets the task
// watchdog; the host backend does nothing.
void portYield(void);

// --- filesystem ----------------------------------------------------------
// Mounts the flash filesystem, formatting it if it has never been used.
// Everything above this line is POSIX: forth_files.cpp uses fopen/opendir and
// needs only the mount point and the usage figures, which no standard call
// supplies portably.
bool portFsMountInit(void);

// The VFS prefix the filesystem is mounted at, e.g. "/littlefs". NULL when
// there is no filesystem on this backend.
const char *portFsMount(void);

bool portFsInfo(uint32_t *used, uint32_t *total);

// --- system --------------------------------------------------------------
uint32_t portFreeHeap(void);
void portRestart(void);
int32_t portRandom(int32_t lo, int32_t hi); // [lo, hi)

#ifdef __cplusplus
}
#endif
