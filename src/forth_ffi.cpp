#include "forth_ffi.h"

#include "port.h"

#include <string.h>

bool (*ffiSymbolLookup)(const char *name, cell *addrOut) = NULL;
void (*ffiSymbolSearch)(const char *substr) = NULL;

static const char *sentinelName = NULL;
static cell sentinelAddr = 0;
static int tableState = 0; // 0 unchecked, 1 good, -1 stale

void ffiSetSentinel(const char *name, cell addr) {
  sentinelName = name;
  sentinelAddr = addr;
  tableState = 0;
}

static void put(const char *s) { portPut(s); }

// ---------------------------------------------------------------- the call

// RV32 ILP32 passes the first eight arguments in a0-a7 and returns in a0, so a
// plain function-pointer cast per arity is the whole trampoline — no assembly,
// and it compiles on the host for testing. Anything wider than eight arguments,
// or floating point, needs a real shim; that is the documented limit.
typedef cell (*fn0)(void);
typedef cell (*fn1)(cell);
typedef cell (*fn2)(cell, cell);
typedef cell (*fn3)(cell, cell, cell);
typedef cell (*fn4)(cell, cell, cell, cell);
typedef cell (*fn5)(cell, cell, cell, cell, cell);
typedef cell (*fn6)(cell, cell, cell, cell, cell, cell);
typedef cell (*fn7)(cell, cell, cell, cell, cell, cell, cell);
typedef cell (*fn8)(cell, cell, cell, cell, cell, cell, cell, cell);

// Jumping to a data address reboots the chip with a backtrace nobody can read,
// and a mistyped `sym` result is the likeliest way to get there. The C3 places
// code in flash-mapped IROM and in IRAM; nothing else is executable.
static bool plausibleCode(cell a) {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  uintptr_t u = (uintptr_t)a;
  // C3 memory map: flash-mapped code, then the masked ROM and IRAM below it.
  if (u >= 0x42000000UL && u < 0x42800000UL) return true; // IROM (flash .text)
  if (u >= 0x40000000UL && u < 0x40060000UL) return true; // masked ROM
  if (u >= 0x4037C000UL && u < 0x403E0000UL) return true; // IRAM
  return false;
#else
  return a != 0; // host build: every address is the linker's business
#endif
}

static void doCall(int argc) {
  cell fn = forthPop();
  if (!plausibleCode(fn)) {
    forthAbort("call: not a code address");
    return;
  }
  cell a[8];
  for (int i = argc - 1; i >= 0; i--) a[i] = forthPop();
  cell r = 0;
  switch (argc) {
    case 0: r = ((fn0)fn)(); break;
    case 1: r = ((fn1)fn)(a[0]); break;
    case 2: r = ((fn2)fn)(a[0], a[1]); break;
    case 3: r = ((fn3)fn)(a[0], a[1], a[2]); break;
    case 4: r = ((fn4)fn)(a[0], a[1], a[2], a[3]); break;
    case 5: r = ((fn5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: r = ((fn6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 7: r = ((fn7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
    case 8: r = ((fn8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
  }
  // Void functions leave a0 undefined, so the result is junk rather than
  // absent. One word with one stack effect is easier to remember than two;
  // `drop` after a void call is the price.
  forthPush(r);
}

static void w_call() {
  cell n = forthPop();
  if (n < 0 || n > 8) {
    forthAbort("call: argument count must be 0..8");
    return;
  }
  doCall((int)n);
}

static void w_call0() { doCall(0); }
static void w_call1() { doCall(1); }
static void w_call2() { doCall(2); }
static void w_call3() { doCall(3); }
static void w_call4() { doCall(4); }

// ------------------------------------------------------------ symbol table

// Checks the table against the one address this firmware knows independently.
// A table built from a different image lists addresses that have all moved;
// calling one of those is an immediate crash, so refusing is the kind option.
static bool tableUsable() {
  if (!ffiSymbolLookup) {
    put("no symbol table loaded (upload data/symbols.txt with `pio run -t uploadfs`)\r\n");
    return false;
  }
  if (tableState == 0) {
    tableState = -1;
    if (sentinelName) {
      cell got = 0;
      if (ffiSymbolLookup(sentinelName, &got) && got == sentinelAddr) tableState = 1;
    } else {
      tableState = 1; // nothing to check against; trust the caller
    }
  }
  if (tableState < 0) {
    put("symbol table is stale (built from a different image) — re-run `pio run -t uploadfs`\r\n");
    return false;
  }
  return true;
}

// ( addr len -- fn | 0 )
static void w_sym() {
  char name[64];
  if (!forthPopString(name, sizeof(name))) return;
  if (!tableUsable()) {
    forthPush(0);
    return;
  }
  cell addr = 0;
  if (!ffiSymbolLookup(name, &addr)) addr = 0;
  forthPush(addr);
}

// ( addr len -- ) prints every symbol whose name contains the substring
static void w_syms() {
  char pat[64];
  if (!forthPopString(pat, sizeof(pat))) return;
  if (!tableUsable()) return;
  if (!ffiSymbolSearch) {
    put("symbol search is not available on this build\r\n");
    return;
  }
  ffiSymbolSearch(pat);
}

// ------------------------------------------------------------------ strings

// A C function wanting `const char *` cannot take Forth's ( addr len ), since
// the text in the string pool is not NUL-terminated. `cstr` copies it somewhere
// that is. Four buffers rotate, so a call can pass several strings at once;
// the fifth overwrites the first, which is the documented limit.
#define CSTR_SLOTS 4
#define CSTR_SIZE 80
static char cstrPool[CSTR_SLOTS][CSTR_SIZE];
static int cstrNext = 0;

// ( addr len -- zaddr )
static void w_cstr() {
  char *dst = cstrPool[cstrNext];
  cstrNext = (cstrNext + 1) % CSTR_SLOTS;
  if (!forthPopString(dst, CSTR_SIZE)) return;
  forthPush((cell)dst);
}

// ( zaddr -- addr len ) the other direction: a C string a call handed back
static void w_zcount() {
  const char *s = (const char *)forthPop();
  if (!s) {
    forthPushString("");
    return;
  }
  forthPushString(s);
}

void ffiRegisterWords() {
  forthAddWord("call", w_call);
  forthAddWord("0call", w_call0);
  forthAddWord("1call", w_call1);
  forthAddWord("2call", w_call2);
  forthAddWord("3call", w_call3);
  forthAddWord("4call", w_call4);
  forthAddWord("sym", w_sym);
  forthAddWord("syms", w_syms);
  forthAddWord("cstr", w_cstr);
  forthAddWord("zcount", w_zcount);
}
