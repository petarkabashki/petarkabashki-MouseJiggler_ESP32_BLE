// The filesystem words, and the FFI's symbol table, over plain POSIX.
//
// Both builds mount LittleFS through a VFS, so fopen/opendir work the same on
// each and this file is shared. The only platform-specific part is the mount
// itself, which is portFsMount() / portFsInfo() in port.h.
//
// Words registered: ls cat rm df include edit append save-to
#pragma once

#include "forth.h"

// Mounts the filesystem (formatting it if it has never been used) and
// registers the words. Safe to call when there is no filesystem: the words are
// still there and report that there is nothing to read.
void forthFilesInit();

bool forthFilesReady();

// True when `name` (relative to the mount point) is a readable file.
bool forthFilesExists(const char *name);

// Runs a file line by line. Definitions are NOT written to the source log —
// the file is already the durable copy. Used for /boot.fs and by `include`.
void forthFilesInclude(const char *path);

// Assign to forthLineSink. While `edit` or `append` is capturing, this takes
// every typed line into the file instead of the interpreter.
bool forthFilesSink(const char *line);

// Wires the FFI's `sym` and `syms` to /symbols.txt. `sentinelName` and
// `sentinelAddr` are passed straight to ffiSetSentinel.
void forthFilesBindSymbols(const char *sentinelName, cell sentinelAddr);
