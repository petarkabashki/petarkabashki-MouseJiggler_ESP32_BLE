// Foreign function interface: calling compiled code from Forth.
//
// The dictionary binding in forth.h is one C++ function per exposed call. This
// is the other end of the trade — no binding at all. Given an address and an
// argument count, `call` marshals the data stack into the platform ABI and
// invokes it, so anything already linked into the image is reachable:
//
//   s" esp_random" sym  0 call  .        \ a function nobody wrote a word for
//
// Addresses come from a symbol table generated at link time and kept on the
// filesystem (tools/gen_symbols.py), so it costs no flash.
#pragma once

#include "forth.h"

// Resolves a symbol name to an address. Returns false when not found. The host
// supplies this — on the device it reads /symbols.txt, on the test harness it
// can be a fixed table. Without one, `sym` reports that no table is loaded.
extern bool (*ffiSymbolLookup)(const char *name, cell *addrOut);

// Prints the symbols whose names contain `substr`. Optional; `syms` says so
// when it is absent.
extern void (*ffiSymbolSearch)(const char *substr);

// A symbol table generated against a different build lists addresses that are
// now wrong, and calling one crashes the chip. Naming a symbol whose address
// this firmware also knows at compile time lets `sym` detect that cheaply:
// if the table disagrees about this one, it is stale and every lookup fails.
void ffiSetSentinel(const char *name, cell addr);

// Registers call / 0call..4call / sym / syms / cstr / zcount.
void ffiRegisterWords();
