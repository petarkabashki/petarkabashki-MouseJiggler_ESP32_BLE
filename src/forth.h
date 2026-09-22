// Tiny Forth interpreter for embedding a command language on the device.
//
// Usage:
//   forthInit();                       // in setup(), after Serial.begin()
//   forthAddWord("move", w_move);      // one line per exposed C++ function
//   forthPoll();                       // in loop(), non-blocking line reader
//
// A native word takes no arguments and communicates through the data stack:
//   void w_move() { cell dy = forthPop(); cell dx = forthPop(); bleMouse.move(dx, dy); }
#pragma once

#include <stdint.h>

class Stream;

// Pointer-sized so that addresses live on the stack unchanged. On the ESP32
// this is exactly int32_t, which is what makes `@` / `!` usable on registers.
typedef intptr_t cell;
typedef void (*ForthFn)();

// Where the REPL reads and writes. Defaults to &Serial.
extern Stream *forthIO;

// Called every few hundred inner-interpreter steps and inside `ms`.
// Point it at a watchdog reset so long-running words cannot trip the WDT.
extern void (*forthYield)();

// Diverts complete input lines somewhere other than the interpreter — a file
// capture, say. Return true to consume the line; the sink then owns the
// prompt. Empty lines are passed through too, so text keeps its blank lines.
extern bool (*forthLineSink)(const char *line);

void forthInit();
void forthPoll();                 // read chars from forthIO, run completed lines
void forthEval(const char *line); // run a string as if typed

// Call once after all native words are registered. Everything defined from
// then on counts as a user definition: `wipe` removes exactly that much, and
// `forget` refuses to cross the line.
void forthMarkCore();

// Definitions typed at the terminal are logged as source text, which is what
// gets persisted. The host owns storage (NVS, a file, ...); these are the hooks.
const char *forthSource();               // NUL-terminated, "" when empty
int forthSourceLen();
void forthLoadSource(const char *text);  // replay definitions, then adopt as the log
void forthClearSource();
bool forthAddWord(const char *name, ForthFn fn, bool immediate = false);
bool forthAddConstant(const char *name, cell value);

// Exposes an existing C++ variable by address. It must be a 32-bit type, since
// `@` and `!` are 32-bit:  forthAddVariable("slow-ms", &cfg.slowInterval);
bool forthAddVariable(const char *name, void *addr);

void forthPush(cell v);
cell forthPop();
cell forthPeek(int fromTop = 0);
int forthDepth();
void forthAbort(const char *msg); // signal an error and unwind to the prompt

// Strings live on the stack as ( addr len ). These are the two helpers a
// device word needs: pop one as a C string, or push one for Forth to print.
bool forthPopString(char *dst, int dstSize); // ( addr len -- ), NUL-terminates
void forthPushString(const char *s);         // ( -- addr len ), copied

// Execution tokens (obtained in Forth with `'`), for host-driven callbacks.
void forthExecuteXt(cell xt);
cell forthFind(const char *name); // xt of a word, or -1; the C++ side of `'`
