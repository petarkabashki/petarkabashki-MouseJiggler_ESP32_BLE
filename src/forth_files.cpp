// The filesystem words, over plain POSIX. See forth_files.h.
//
// Both builds mount LittleFS through ESP-IDF's VFS layer, so fopen/opendir
// behave identically and this file is shared rather than written twice. Paths
// the user types are relative to the mount point: `s" boot.fs" cat` reads
// /littlefs/boot.fs.
//
// The C3's USB is a fixed-function CDC/JTAG block, not an OTG controller, so
// the board cannot enumerate as a mass-storage device the way an S2/S3 can;
// files are moved over this terminal instead, with `edit` and `cat`.

#include "forth_files.h"
#include "forth_ffi.h"
#include "port.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYMBOL_FILE "symbols.txt"
#define PATH_MAX_ 128

static bool fsReady;

bool forthFilesReady() { return fsReady; }

// Turns a user-typed name into an absolute VFS path. A leading slash is the
// user's own business only in the sense that it is ignored — there is one
// filesystem and everything lives under its mount point.
static const char *fsPath(char *buf, int n, const char *name) {
  const char *mount = portFsMount();
  while (*name == '/') name++;
  snprintf(buf, n, "%s/%s", mount ? mount : "", name);
  return buf;
}

bool forthFilesExists(const char *name) {
  if (!fsReady) return false;
  char path[PATH_MAX_];
  fsPath(path, sizeof(path), name);
  struct stat st;
  return stat(path, &st) == 0;
}

// Pops ( addr len ) and turns it into an absolute path.
static bool popPath(char *buf, int n) {
  char name[64];
  if (!forthPopString(name, sizeof(name))) return false;
  if (!fsReady) { forthAbort("no filesystem"); return false; }
  fsPath(buf, n, name);
  return true;
}

static void printUsage() {
  uint32_t used = 0, total = 0;
  if (portFsInfo(&used, &total))
    portPrintf("  %u used / %u total bytes\r\n", (unsigned)used, (unsigned)total);
  else
    portPut("  (usage unavailable)\r\n");
}

static void fw_ls() {
  if (!fsReady) { forthAbort("no filesystem"); return; }
  const char *mount = portFsMount();
  DIR *dir = opendir(mount);
  if (!dir) { forthAbort("cannot read directory"); return; }
  char path[PATH_MAX_];
  int n = 0;
  for (struct dirent *e = readdir(dir); e; e = readdir(dir)) {
    struct stat st;
    // Explicit precisions: the compiler cannot see that a mount point and a
    // LittleFS name are both short, and -Werror=format-truncation is on.
    snprintf(path, sizeof(path), "%.24s/%.90s", mount, e->d_name);
    unsigned size = stat(path, &st) == 0 ? (unsigned)st.st_size : 0;
    portPrintf("  %-24s %6u\r\n", e->d_name, size);
    n++;
  }
  closedir(dir);
  portPrintf("  %d file(s)\r\n", n);
  printUsage();
}

static void fw_cat() { // ( addr len -- )
  char path[PATH_MAX_];
  if (!popPath(path, sizeof(path))) return;
  FILE *f = fopen(path, "r");
  if (!f) { forthAbort("no such file"); return; }
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (c == '\n') portPut("\r\n"); else portPutc((char)c);
    if ((c & 0x3f) == 0) portYield();
  }
  fclose(f);
  portPut("\r\n");
}

static void fw_rm() { // ( addr len -- )
  char path[PATH_MAX_];
  if (!popPath(path, sizeof(path))) return;
  if (unlink(path) != 0) forthAbort("could not remove");
}

static void fw_df() {
  if (!fsReady) { forthAbort("no filesystem"); return; }
  printUsage();
}

// ---------------------------------------------------------------------------
// Symbol table for the FFI. tools/gen_symbols.py writes data/symbols.txt after
// each link; `pio run -t uploadfs` puts it here. Lines are "hexaddr name",
// sorted by name. A few hundred KB, scanned linearly — one lookup costs about
// a second, which is why the idiom is to resolve once into a constant:
//
//   s" esp_random" sym constant &rnd     &rnd 0call .
// ---------------------------------------------------------------------------

// Walks the table, handing each (name, addr) to `visit`. Returning false from
// `visit` stops the scan. Reading line by line through fgets would be a syscall
// per line over ten thousand lines, so this buffers by hand.
static bool symScan(bool (*visit)(const char *name, cell addr, void *ctx), void *ctx) {
  if (!fsReady) return false;
  char path[PATH_MAX_];
  fsPath(path, sizeof(path), SYMBOL_FILE);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char chunk[256];
  char line[128];
  int len = 0;
  bool overflow = false;
  bool stopped = false;
  bool eof = false;
  while (!stopped && !eof) {
    size_t got = fread(chunk, 1, sizeof(chunk), f);
    if (got == 0) {
      // A file with no trailing newline still has one last line to deliver.
      eof = true;
      chunk[0] = '\n';
      got = 1;
    }
    for (size_t i = 0; i < got; i++) {
      char c = chunk[i];
      if (c != '\n' && c != '\r') {
        if (len < (int)sizeof(line) - 1) line[len++] = c; else overflow = true;
        continue;
      }
      if (len && !overflow) {
        line[len] = 0;
        char *sp = strchr(line, ' ');
        if (sp) {
          *sp = 0;
          cell addr = (cell)strtoul(line, NULL, 16);
          if (!visit(sp + 1, addr, ctx)) { stopped = true; break; }
        }
      }
      len = 0;
      overflow = false;
    }
    portYield();
  }
  fclose(f);
  return true;
}

struct SymFind { const char *want; cell addr; bool found; };

static bool symFindVisit(const char *name, cell addr, void *ctx) {
  SymFind *s = (SymFind *)ctx;
  if (strcmp(name, s->want) != 0) return true;
  s->addr = addr;
  s->found = true;
  return false;
}

static bool symLookup(const char *name, cell *addrOut) {
  SymFind s = { name, 0, false };
  if (!symScan(symFindVisit, &s) || !s.found) return false;
  *addrOut = s.addr;
  return true;
}

struct SymSearch { const char *pat; int hits; };

static bool symSearchVisit(const char *name, cell addr, void *ctx) {
  SymSearch *s = (SymSearch *)ctx;
  if (!strstr(name, s->pat)) return true;
  portPrintf("  %08lx %s\r\n", (unsigned long)addr, name);
  // A one-letter pattern would otherwise scroll the whole table past.
  return ++s->hits < 60;
}

static void symSearch(const char *pat) {
  SymSearch s = { pat, 0 };
  symScan(symSearchVisit, &s);
  portPrintf("  %d match%s%s\r\n", s.hits, s.hits == 1 ? "" : "es",
             s.hits >= 60 ? " (stopped at 60)" : "");
}

void forthFilesBindSymbols(const char *sentinelName, cell sentinelAddr) {
  ffiSymbolLookup = symLookup;
  ffiSymbolSearch = symSearch;
  ffiSetSentinel(sentinelName, sentinelAddr);
}

// --- include ---------------------------------------------------------------
static int includeDepth = 0;

void forthFilesInclude(const char *name) {
  if (!fsReady) { forthAbort("no filesystem"); return; }
  if (includeDepth >= 4) { forthAbort("includes nested too deeply"); return; }
  char path[PATH_MAX_];
  fsPath(path, sizeof(path), name);
  FILE *f = fopen(path, "r");
  if (!f) { portPrintf("?? no such file: %s\r\n", path); return; }
  includeDepth++;
  char line[192];
  int lineNo = 0;
  while (fgets(line, sizeof(line), f)) {
    lineNo++;
    portYield();
    // fgets keeps the newline; a line longer than the buffer arrives in pieces,
    // which would evaluate as nonsense, so it is skipped whole.
    size_t n = strlen(line);
    bool truncated = n == sizeof(line) - 1 && line[n - 1] != '\n';
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (truncated) {
      portPrintf("?? %s:%d line too long, skipped\r\n", path, lineNo);
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') { }
      continue;
    }
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) continue;
    forthEval(s);
  }
  includeDepth--;
  fclose(f);
}

static void fw_include() { // ( addr len -- )
  char name[64];
  if (!forthPopString(name, sizeof(name))) return;
  forthFilesInclude(name);
}

// --- capturing typed text into a file -------------------------------------
// `s" boot.fs" edit` sends every following line to the file until a line that
// is just `;;`. This is the interpreter's forthLineSink hook, so the lines
// never reach the evaluator.
static FILE *captureFile;
static char capturePath[PATH_MAX_];

bool forthFilesSink(const char *line) {
  if (!captureFile) return false;
  if (strcmp(line, ";;") == 0) {
    long n = ftell(captureFile);
    fclose(captureFile);
    captureFile = NULL;
    portPrintf("wrote %s (%u bytes)\r\nok> ", capturePath, (unsigned)(n < 0 ? 0 : n));
    return true;
  }
  fputs(line, captureFile);
  fputc('\n', captureFile);
  portPut("... ");
  return true;
}

static void startCapture(const char *path, const char *mode) {
  captureFile = fopen(path, mode);
  if (!captureFile) { forthAbort("could not open for writing"); return; }
  snprintf(capturePath, sizeof(capturePath), "%s", path);
  portPrintf("capturing to %s — end with a line containing just  ;;\r\n... ", path);
}

static void fw_edit()   { char p[PATH_MAX_]; if (popPath(p, sizeof(p))) startCapture(p, "w"); }
static void fw_append() { char p[PATH_MAX_]; if (popPath(p, sizeof(p))) startCapture(p, "a"); }

// Dumps the definitions typed this session into a file, so an experiment at
// the prompt can become a boot script without retyping it.
static void fw_saveTo() { // ( addr len -- )
  char path[PATH_MAX_];
  if (!popPath(path, sizeof(path))) return;
  FILE *f = fopen(path, "w");
  if (!f) { forthAbort("could not open for writing"); return; }
  fputs(forthSource(), f);
  fclose(f);
  portPrintf("wrote %s (%d bytes)\r\n", path, forthSourceLen());
}

void forthFilesInit() {
  fsReady = portFsMountInit();
  if (!fsReady) portPut("filesystem unavailable\r\n");

  forthAddWord("ls", fw_ls);
  forthAddWord("cat", fw_cat);
  forthAddWord("include", fw_include);
  forthAddWord("edit", fw_edit);
  forthAddWord("append", fw_append);
  forthAddWord("rm", fw_rm);
  forthAddWord("df", fw_df);
  forthAddWord("save-to", fw_saveTo);
}
