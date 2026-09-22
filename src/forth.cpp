#include "port.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "forth.h"

// ---------------------------------------------------------------------------
// Limits. All storage is static; the interpreter never allocates.
// ---------------------------------------------------------------------------
#define DICT_MAX    320
#define CODE_MAX    3072
#define NAME_POOL   4096
#define DATA_CELLS  128
#define DSTACK_MAX  48
#define RSTACK_MAX  48
#define CFSTACK_MAX 16
#define STR_POOL    512
#define SRC_LOG     3072
#define FORTH_LINE_MAX    160

// Pseudo-opcodes stored in the code space. Real words are dictionary indices,
// which are always >= 0, so negative values are free for the inner interpreter.
enum {
  OP_EXIT    = -1,
  OP_LIT     = -2,
  OP_BRANCH  = -3,
  OP_ZBRANCH = -4,
  OP_DO      = -5,
  OP_LOOP    = -6,
  OP_LEAVE   = -7,
  OP_STR     = -8,
};

enum {
  F_IMMEDIATE = 1,
  F_NATIVE    = 2,
  F_VAR       = 4, // val holds the address of a data-space cell
  F_CONST     = 8, // val holds the value itself
};

struct DictEntry {
  const char *name;
  ForthFn fn;
  cell val;
  uint16_t addr;
  uint8_t flags;
  // Allocation watermarks as they stood just before this entry was created.
  // `forget` rewinds every pool to these, so removing a word frees its code,
  // its name, its data and the source line that defined it.
  uint16_t codeMark, nameMark, dataMark, strMark, srcMark;
};

static DictEntry dict[DICT_MAX];
static int dictCount = 0;

static cell code[CODE_MAX];
static uint16_t codeHere = 0;

static char namePool[NAME_POOL];
static uint16_t nameHere = 0;

static cell dataSpace[DATA_CELLS];
static uint16_t dataHere = 0;

static char strPool[STR_POOL];
static uint16_t strHere = 0;

// Source log: the text of every line that defined something, replayed at boot.
static char srcLog[SRC_LOG];
static uint16_t srcHere = 0;

// Watermarks for `wipe`, set by forthMarkCore().
static int coreCount = 0;
static uint16_t coreCode = 0, coreName = 0, coreData = 0, coreStr = 0;

static cell dstack[DSTACK_MAX];
static int dsp = 0;
static cell rstack[RSTACK_MAX];
static int rsp = 0;
static uint16_t cfstack[CFSTACK_MAX];
static int cfsp = 0;

static uint16_t ip = 0;        // instruction pointer into code[]
static bool compiling = false; // true between : and ;
static bool aborting = false;
static int numBase = 10;

static const char *inPtr = NULL; // tokenizer cursor into the current line

// Snapshot taken at `:` so a failed definition can be rolled back.
static int defDict = 0;
static uint16_t defCode = 0, defName = 0;

Stream *forthIO = NULL;
void (*forthYield)() = NULL;
bool (*forthLineSink)(const char *) = NULL;

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
static void put(const char *s) { portPut(s); }
static void putc_(char c) { portPutc(c); }
static void putn(cell v) {
  if (numBase == 16) portPrintf("$%lx", (unsigned long)(uint32_t)v);
  else               portPrintf("%ld", (long)v);
}

void forthAbort(const char *msg) {
  if (!aborting) {
    put("\r\n?? ");
    put(msg);
    put("\r\n");
  }
  aborting = true;
}

// ---------------------------------------------------------------------------
// Stacks
// ---------------------------------------------------------------------------
void forthPush(cell v) {
  if (dsp >= DSTACK_MAX) { forthAbort("stack overflow"); return; }
  dstack[dsp++] = v;
}

cell forthPop() {
  if (dsp <= 0) { forthAbort("stack underflow"); return 0; }
  return dstack[--dsp];
}

cell forthPeek(int fromTop) {
  if (dsp - 1 - fromTop < 0) { forthAbort("stack underflow"); return 0; }
  return dstack[dsp - 1 - fromTop];
}

int forthDepth() { return dsp; }

static void rpush(cell v) {
  if (rsp >= RSTACK_MAX) { forthAbort("return stack overflow"); return; }
  rstack[rsp++] = v;
}

static cell rpop() {
  if (rsp <= 0) { forthAbort("return stack underflow"); return 0; }
  return rstack[--rsp];
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------
static char tokenBuf[48];

// Returns the next blank-delimited token, or NULL at end of line.
static const char *nextToken() {
  if (!inPtr) return NULL;
  while (*inPtr == ' ' || *inPtr == '\t') inPtr++;
  if (!*inPtr) return NULL;
  int n = 0;
  while (*inPtr && *inPtr != ' ' && *inPtr != '\t') {
    if (n < (int)sizeof(tokenBuf) - 1) tokenBuf[n++] = *inPtr;
    inPtr++;
  }
  tokenBuf[n] = 0;
  return tokenBuf;
}

// Consumes text up to (and including) `delim`; returns it without the delim.
static const char *parseUntil(char delim) {
  if (!inPtr) return NULL;
  if (*inPtr == ' ') inPtr++;
  int n = 0;
  while (*inPtr && *inPtr != delim) {
    if (n < (int)sizeof(tokenBuf) - 1) tokenBuf[n++] = *inPtr;
    inPtr++;
  }
  if (*inPtr == delim) inPtr++;
  tokenBuf[n] = 0;
  return tokenBuf;
}

// ---------------------------------------------------------------------------
// Dictionary
// ---------------------------------------------------------------------------
static bool sameWord(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == *b;
}

// Newest definition wins, so a word can be redefined from the terminal.
static int find(const char *name) {
  for (int i = dictCount - 1; i >= 0; i--)
    if (sameWord(dict[i].name, name)) return i;
  return -1;
}

static const char *internName(const char *name) {
  int len = strlen(name);
  if (nameHere + len + 1 > NAME_POOL) { forthAbort("name pool full"); return NULL; }
  char *dst = &namePool[nameHere];
  memcpy(dst, name, len + 1);
  nameHere += len + 1;
  return dst;
}

static int newEntry(const char *name, bool copyName) {
  if (dictCount >= DICT_MAX) { forthAbort("dictionary full"); return -1; }
  uint16_t nameMark = nameHere;
  const char *stored = copyName ? internName(name) : name;
  if (!stored) return -1;
  DictEntry &e = dict[dictCount];
  e.name = stored;
  e.fn = NULL;
  e.val = 0;
  e.addr = 0;
  e.flags = 0;
  e.codeMark = codeHere;
  e.nameMark = nameMark;
  e.dataMark = dataHere;
  e.strMark = strHere;
  e.srcMark = srcHere;
  return dictCount++;
}

bool forthAddWord(const char *name, ForthFn fn, bool immediate) {
  int i = newEntry(name, false);
  if (i < 0) return false;
  dict[i].fn = fn;
  dict[i].flags = F_NATIVE | (immediate ? F_IMMEDIATE : 0);
  return true;
}

bool forthAddConstant(const char *name, cell value) {
  int i = newEntry(name, false);
  if (i < 0) return false;
  dict[i].val = value;
  dict[i].flags = F_CONST;
  return true;
}

bool forthAddVariable(const char *name, void *addr) {
  int i = newEntry(name, false);
  if (i < 0) return false;
  dict[i].val = (cell)(uintptr_t)addr;
  dict[i].flags = F_VAR;
  return true;
}

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------
static void emit(cell v) {
  if (codeHere >= CODE_MAX) { forthAbort("code space full"); return; }
  code[codeHere++] = v;
}

static void cfpush(uint16_t v) {
  if (cfsp >= CFSTACK_MAX) { forthAbort("control flow nesting too deep"); return; }
  cfstack[cfsp++] = v;
}

static uint16_t cfpop() {
  if (cfsp <= 0) { forthAbort("unstructured control flow"); return 0; }
  return cfstack[--cfsp];
}

static bool parseNumber(const char *s, cell *out) {
  int base = numBase;
  bool neg = false;
  if (*s == '-' && s[1]) { neg = true; s++; }
  if (s[0] == '$') { base = 16; s++; }
  else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
  else if (s[0] == '#') { base = 10; s++; }
  else if (s[0] == '%') { base = 2; s++; }
  else if (s[0] == '\'' && s[1] && s[2] == '\'' && !s[3]) { *out = (cell)s[1]; return true; }
  if (!*s) return false;
  char *end = NULL;
  long v = strtol(s, &end, base);
  if (!end || *end) return false;
  *out = (cell)(neg ? -v : v);
  return true;
}

// ---------------------------------------------------------------------------
// Inner interpreter
// ---------------------------------------------------------------------------
static void execute(int w);

static void run(uint16_t start) {
  uint16_t savedIp = ip;
  int base = rsp;
  unsigned steps = 0;
  ip = start;

  while (!aborting) {
    if (ip >= CODE_MAX) { forthAbort("ip out of range"); break; }
    if ((++steps & 0xFF) == 0 && forthYield) forthYield();

    cell x = code[ip++];
    if (x >= 0) {
      DictEntry &e = dict[x];
      if (e.flags & (F_VAR | F_CONST)) {
        forthPush(e.val);
      } else if (e.flags & F_NATIVE) {
        e.fn();
      } else {
        rpush((cell)ip);
        ip = e.addr;
      }
      continue;
    }

    switch (x) {
      case OP_EXIT:
        if (rsp <= base) { ip = savedIp; return; }
        ip = (uint16_t)rpop();
        break;
      case OP_LIT:
        forthPush(code[ip++]);
        break;
      case OP_STR:
        put((const char *)(uintptr_t)code[ip++]);
        break;
      case OP_BRANCH:
        ip = (uint16_t)code[ip];
        break;
      case OP_ZBRANCH:
        if (forthPop() == 0) ip = (uint16_t)code[ip];
        else ip++;
        break;
      case OP_DO: {
        cell exitAddr = code[ip++];
        cell index = forthPop();
        cell limit = forthPop();
        rpush(limit);
        rpush(index);
        rpush(exitAddr);
        break;
      }
      case OP_LOOP: {
        cell bodyStart = code[ip];
        if (rsp < base + 3) { forthAbort("loop without do"); break; }
        cell index = rstack[rsp - 2] + 1;
        cell limit = rstack[rsp - 3];
        if (index < limit) {
          rstack[rsp - 2] = index;
          ip = (uint16_t)bodyStart;
        } else {
          rsp -= 3;
          ip++;
        }
        break;
      }
      case OP_LEAVE: {
        if (rsp < base + 3) { forthAbort("leave without do"); break; }
        ip = (uint16_t)rstack[rsp - 1];
        rsp -= 3;
        break;
      }
      default:
        forthAbort("bad opcode");
        break;
    }
  }
  ip = savedIp;
}

static void execute(int w) {
  if (w < 0 || w >= dictCount) { forthAbort("bad execution token"); return; }
  DictEntry &e = dict[w];
  if (e.flags & (F_VAR | F_CONST)) forthPush(e.val);
  else if (e.flags & F_NATIVE) e.fn();
  else run(e.addr);
}

void forthExecuteXt(cell xt) { execute((int)xt); }

// Lets the host cache the xt of a word defined in Forth, so C++ can call into
// a script: `tickXt = forthFind("tick");` then forthExecuteXt(tickXt).
cell forthFind(const char *name) { return (cell)find(name); }

// ---------------------------------------------------------------------------
// Core words
// ---------------------------------------------------------------------------
#define BINOP(name, expr)                \
  static void name() {                   \
    cell b = forthPop(), a = forthPop();  \
    forthPush(expr);                     \
  }

static void w_dup()  { cell a = forthPeek(); forthPush(a); }
static void w_qdup() { if (dsp && forthPeek()) { cell a = forthPeek(); forthPush(a); } }
static void w_drop() { forthPop(); }
static void w_swap() { cell b = forthPop(), a = forthPop(); forthPush(b); forthPush(a); }
static void w_over() { cell a = forthPeek(1); forthPush(a); }
static void w_nip()  { cell b = forthPop(); forthPop(); forthPush(b); }
static void w_tuck() { cell b = forthPop(), a = forthPop(); forthPush(b); forthPush(a); forthPush(b); }
static void w_rot()  { cell c = forthPop(), b = forthPop(), a = forthPop(); forthPush(b); forthPush(c); forthPush(a); }
static void w_depth() { forthPush(dsp); }
static void w_clear() { dsp = 0; }

BINOP(w_add, a + b)
BINOP(w_sub, a - b)
BINOP(w_mul, a * b)
BINOP(w_and, a & b)
BINOP(w_or,  a | b)
BINOP(w_xor, a ^ b)
BINOP(w_lsh, a << b)
BINOP(w_rsh, (cell)((uint32_t)a >> b))
BINOP(w_min, a < b ? a : b)
BINOP(w_max, a > b ? a : b)
BINOP(w_eq,  a == b ? -1 : 0)
BINOP(w_ne,  a != b ? -1 : 0)
BINOP(w_lt,  a < b ? -1 : 0)
BINOP(w_gt,  a > b ? -1 : 0)
BINOP(w_le,  a <= b ? -1 : 0)
BINOP(w_ge,  a >= b ? -1 : 0)

static void w_div() { cell b = forthPop(), a = forthPop(); if (!b) { forthAbort("divide by zero"); return; } forthPush(a / b); }
static void w_mod() { cell b = forthPop(), a = forthPop(); if (!b) { forthAbort("divide by zero"); return; } forthPush(a % b); }
static void w_neg() { forthPush(-forthPop()); }
static void w_abs() { cell a = forthPop(); forthPush(a < 0 ? -a : a); }
static void w_inv() { forthPush(~forthPop()); }
static void w_zeq() { forthPush(forthPop() == 0 ? -1 : 0); }
static void w_zlt() { forthPush(forthPop() < 0 ? -1 : 0); }
static void w_1add() { forthPush(forthPop() + 1); }
static void w_1sub() { forthPush(forthPop() - 1); }

static bool validAddr(cell a, int align) {
  if (a == 0) { forthAbort("null address"); return false; }
  if (align > 1 && (a & (align - 1))) { forthAbort("misaligned address"); return false; }
  return true;
}

static void w_fetch()  { cell a = forthPop(); if (validAddr(a, 4)) forthPush((cell)*(volatile uint32_t *)(uintptr_t)a); }
static void w_store()  { cell a = forthPop(), v = forthPop(); if (validAddr(a, 4)) *(volatile uint32_t *)(uintptr_t)a = (uint32_t)v; }
static void w_cfetch() { cell a = forthPop(); if (validAddr(a, 1)) forthPush((cell)*(volatile uint8_t *)(uintptr_t)a); }
static void w_cstore() { cell a = forthPop(), v = forthPop(); if (validAddr(a, 1)) *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v; }
static void w_plusStore() { cell a = forthPop(), v = forthPop(); if (validAddr(a, 4)) *(volatile uint32_t *)(uintptr_t)a += (uint32_t)v; }
static void w_bis() { cell a = forthPop(), v = forthPop(); if (validAddr(a, 4)) *(volatile uint32_t *)(uintptr_t)a |= (uint32_t)v; }
static void w_bic() { cell a = forthPop(), v = forthPop(); if (validAddr(a, 4)) *(volatile uint32_t *)(uintptr_t)a &= ~(uint32_t)v; }

static void w_cells()  { forthPush(forthPop() * (cell)sizeof(cell)); }
static void w_cellPlus() { forthPush(forthPop() + (cell)sizeof(cell)); }

static void w_dot()   { putn(forthPop()); putc_(' '); }
static void w_uDot()  { portPrintf("%lu", (unsigned long)(uint32_t)forthPop()); putc_(' '); }
static void w_emit()  { putc_((char)forthPop()); }
static void w_cr()    { put("\r\n"); }
static void w_space() { putc_(' '); }
static void w_question() { cell a = forthPop(); if (validAddr(a, 4)) { putn((cell)*(volatile uint32_t *)(uintptr_t)a); putc_(' '); } }
static void w_hex()   { numBase = 16; }
static void w_dec()   { numBase = 10; }

static void w_dotS() {
  put("<");
  portPrintf("%d", dsp);
  put("> ");
  for (int i = 0; i < dsp; i++) { putn(dstack[i]); putc_(' '); }
  put("\r\n");
}

static void w_words() {
  int col = 0;
  for (int i = 0; i < dictCount; i++) {
    put(dict[i].name);
    putc_(' ');
    if (++col % 8 == 0) put("\r\n");
  }
  put("\r\n");
}

static void w_free() {
  portPrintf("dict %d/%d  code %u/%d  names %u/%d  data %u/%d  strings %u/%d\r\n",
               dictCount, DICT_MAX, (unsigned)codeHere, CODE_MAX,
               (unsigned)nameHere, NAME_POOL, (unsigned)dataHere, DATA_CELLS,
               (unsigned)strHere, STR_POOL);
}

// ---------------------------------------------------------------------------
// Strings
//
// A string on the stack is ( addr len ). Literals from s" live in the name
// pool (compiled) or in a small ring of transient buffers (interpreted), so a
// freshly typed literal stays valid for the rest of the line.
//
// A string *buffer* created by `string` has the layout
//   uint16 capacity | uint16 length | bytes...
// and its word pushes the header address. All writes truncate rather than
// abort, so overlong text never takes the terminal down.
// ---------------------------------------------------------------------------
#define TRANSIENT_COUNT 4
#define TRANSIENT_SIZE  80

static char transientBuf[TRANSIENT_COUNT][TRANSIENT_SIZE];
static uint8_t transientIdx = 0;

static char *nextTransient() {
  char *p = transientBuf[transientIdx];
  transientIdx = (transientIdx + 1) % TRANSIENT_COUNT;
  return p;
}

struct StrBuf {
  uint16_t capacity;
  uint16_t length;
  char data[1];
};

static StrBuf *asStrBuf(cell a) {
  if (!a) { forthAbort("null string"); return NULL; }
  StrBuf *s = (StrBuf *)(uintptr_t)a;
  if ((char *)s < strPool || (char *)s >= strPool + STR_POOL) {
    forthAbort("not a string buffer");
    return NULL;
  }
  return s;
}

void forthPushString(const char *s) {
  int len = strlen(s);
  if (len > TRANSIENT_SIZE - 1) len = TRANSIENT_SIZE - 1;
  char *dst = nextTransient();
  memcpy(dst, s, len);
  dst[len] = 0;
  forthPush((cell)(uintptr_t)dst);
  forthPush(len);
}

bool forthPopString(char *dst, int dstSize) {
  cell len = forthPop();
  cell addr = forthPop();
  if (aborting || !dst || dstSize <= 0) return false;
  if (!addr) { forthAbort("null string"); return false; }
  if (len < 0) len = 0;
  if (len > dstSize - 1) len = dstSize - 1;
  memcpy(dst, (const char *)(uintptr_t)addr, len);
  dst[len] = 0;
  return true;
}

// ( -- addr len )  s" some text"
static void w_sQuote() {
  const char *s = parseUntil('"');
  int len = strlen(s);
  if (compiling) {
    const char *stored = internName(s);
    if (!stored) return;
    emit(OP_LIT); emit((cell)(uintptr_t)stored);
    emit(OP_LIT); emit(len);
  } else {
    if (len > TRANSIENT_SIZE - 1) len = TRANSIENT_SIZE - 1;
    char *dst = nextTransient();
    memcpy(dst, s, len);
    dst[len] = 0;
    forthPush((cell)(uintptr_t)dst);
    forthPush(len);
  }
}

// ( addr len -- )
static void w_type() {
  cell len = forthPop(), addr = forthPop();
  if (aborting || !addr) return;
  const char *p = (const char *)(uintptr_t)addr;
  for (cell i = 0; i < len; i++) putc_(p[i]);
}

// ( n -- ) name follows: reserves an n-byte string buffer
static void w_string() {
  const char *name = nextToken();
  cell n = forthPop();
  if (!name) { forthAbort("name expected"); return; }
  if (n <= 0 || n > 1024) { forthAbort("bad string size"); return; }
  uint16_t need = (uint16_t)(4 + n + 1);
  need = (need + 3) & ~3u; // keep following buffers 4-byte aligned
  if (strHere + need > STR_POOL) { forthAbort("string pool full"); return; }
  int i = newEntry(name, true);
  if (i < 0) return;
  StrBuf *s = (StrBuf *)&strPool[strHere];
  s->capacity = (uint16_t)n;
  s->length = 0;
  s->data[0] = 0;
  dict[i].val = (cell)(uintptr_t)s;
  dict[i].flags = F_VAR;
  strHere += need;
}

static void appendTo(StrBuf *s, const char *src, int len) {
  if (len < 0) len = 0;
  int room = (int)s->capacity - (int)s->length;
  if (len > room) len = room; // graceful truncation
  memcpy(s->data + s->length, src, len);
  s->length += len;
  s->data[s->length] = 0;
}

// ( addr len dest -- )
static void w_sStore() {
  StrBuf *s = asStrBuf(forthPop());
  cell len = forthPop(), addr = forthPop();
  if (!s || aborting) return;
  s->length = 0;
  appendTo(s, (const char *)(uintptr_t)addr, (int)len);
}

// ( addr len dest -- )
static void w_sAppend() {
  StrBuf *s = asStrBuf(forthPop());
  cell len = forthPop(), addr = forthPop();
  if (!s || aborting) return;
  appendTo(s, (const char *)(uintptr_t)addr, (int)len);
}

// ( c dest -- )
static void w_sAppendChar() {
  StrBuf *s = asStrBuf(forthPop());
  cell c = forthPop();
  if (!s || aborting) return;
  char ch = (char)c;
  appendTo(s, &ch, 1);
}

// ( n dest -- )  appends the number in the current base
static void w_sAppendNum() {
  StrBuf *s = asStrBuf(forthPop());
  cell n = forthPop();
  if (!s || aborting) return;
  char tmp[16];
  if (numBase == 16) snprintf(tmp, sizeof(tmp), "%lx", (unsigned long)(uint32_t)n);
  else snprintf(tmp, sizeof(tmp), "%ld", (long)n);
  appendTo(s, tmp, strlen(tmp));
}

// ( dest -- addr len )
static void w_sFetch() {
  StrBuf *s = asStrBuf(forthPop());
  if (!s) return;
  forthPush((cell)(uintptr_t)s->data);
  forthPush(s->length);
}

static void w_sLen()   { StrBuf *s = asStrBuf(forthPop()); if (s) forthPush(s->length); }
static void w_sRoom()  { StrBuf *s = asStrBuf(forthPop()); if (s) forthPush(s->capacity - s->length); }
static void w_sClear() { StrBuf *s = asStrBuf(forthPop()); if (s) { s->length = 0; s->data[0] = 0; } }
static void w_sPrint() { StrBuf *s = asStrBuf(forthPop()); if (s) put(s->data); }

// ( a1 u1 a2 u2 -- flag )
static void w_sEqual() {
  cell u2 = forthPop(), a2 = forthPop(), u1 = forthPop(), a1 = forthPop();
  if (aborting) return;
  bool eq = (u1 == u2) && (u1 == 0 ||
             memcmp((const void *)(uintptr_t)a1, (const void *)(uintptr_t)a2, (size_t)u1) == 0);
  forthPush(eq ? -1 : 0);
}

// ( addr len n -- addr' len' )  drop the first n characters
static void w_slashString() {
  cell n = forthPop(), len = forthPop(), addr = forthPop();
  if (aborting) return;
  if (n < 0) n = 0;
  if (n > len) n = len;
  forthPush(addr + n);
  forthPush(len - n);
}

// --- return stack ---
static void w_toR()   { rpush(forthPop()); }
static void w_rFrom() { forthPush(rpop()); }
static void w_rAt()   { if (rsp <= 0) { forthAbort("return stack underflow"); return; } forthPush(rstack[rsp - 1]); }
static void w_i()     { if (rsp < 2) { forthAbort("i outside loop"); return; } forthPush(rstack[rsp - 2]); }
static void w_j()     { if (rsp < 5) { forthAbort("j outside outer loop"); return; } forthPush(rstack[rsp - 5]); }

// --- defining words ---
static void w_colon() {
  const char *name = nextToken();
  if (!name) { forthAbort("name expected after :"); return; }
  defDict = dictCount;
  defCode = codeHere;
  defName = nameHere;
  int i = newEntry(name, true);
  if (i < 0) return;
  dict[i].addr = codeHere;
  compiling = true;
}

static void w_semicolon() {
  emit(OP_EXIT);
  compiling = false;
  if (cfsp != 0) { cfsp = 0; forthAbort("unbalanced control flow in definition"); }
}

static void w_immediate() {
  if (dictCount > 0) dict[dictCount - 1].flags |= F_IMMEDIATE;
}

static void w_variable() {
  const char *name = nextToken();
  if (!name) { forthAbort("name expected"); return; }
  if (dataHere >= DATA_CELLS) { forthAbort("data space full"); return; }
  int i = newEntry(name, true);
  if (i < 0) return;
  dataSpace[dataHere] = 0;
  dict[i].val = (cell)(uintptr_t)&dataSpace[dataHere];
  dict[i].flags = F_VAR;
  dataHere++;
}

// ( n -- ) `10 array buf` reserves 10 cells; buf pushes their base address.
static void w_array() {
  const char *name = nextToken();
  cell n = forthPop();
  if (!name) { forthAbort("name expected"); return; }
  if (n <= 0 || dataHere + n > DATA_CELLS) { forthAbort("data space full"); return; }
  int i = newEntry(name, true);
  if (i < 0) return;
  for (cell k = 0; k < n; k++) dataSpace[dataHere + k] = 0;
  dict[i].val = (cell)(uintptr_t)&dataSpace[dataHere];
  dict[i].flags = F_VAR;
  dataHere += (uint16_t)n;
}

static void w_constant() {
  const char *name = nextToken();
  cell v = forthPop();
  if (!name) { forthAbort("name expected"); return; }
  int i = newEntry(name, true);
  if (i < 0) return;
  dict[i].val = v;
  dict[i].flags = F_CONST;
}

static void w_tick() {
  const char *name = nextToken();
  if (!name) { forthAbort("name expected after '"); return; }
  int w = find(name);
  if (w < 0) { forthAbort("undefined word"); return; }
  if (compiling) { emit(OP_LIT); emit(w); }
  else forthPush(w);
}

static void w_execute() { execute((int)forthPop()); }

// --- control flow (all immediate) ---
static void w_if()    { emit(OP_ZBRANCH); cfpush(codeHere); emit(0); }
static void w_else()  { uint16_t p = cfpop(); emit(OP_BRANCH); cfpush(codeHere); emit(0); code[p] = codeHere; }
static void w_then()  { uint16_t p = cfpop(); code[p] = codeHere; }
static void w_begin() { cfpush(codeHere); }
static void w_again() { uint16_t s = cfpop(); emit(OP_BRANCH); emit(s); }
static void w_until() { uint16_t s = cfpop(); emit(OP_ZBRANCH); emit(s); }
static void w_while() { uint16_t s = cfpop(); emit(OP_ZBRANCH); cfpush(codeHere); emit(0); cfpush(s); }
static void w_repeat() { uint16_t s = cfpop(); uint16_t p = cfpop(); emit(OP_BRANCH); emit(s); code[p] = codeHere; }
static void w_do()    { emit(OP_DO); cfpush(codeHere); emit(0); }
static void w_loop()  { uint16_t p = cfpop(); emit(OP_LOOP); emit(p + 1); code[p] = codeHere; }
static void w_leave() { emit(OP_LEAVE); }

// ---------------------------------------------------------------------------
// Vocabulary: inspect, decompile, forget, wipe
// ---------------------------------------------------------------------------
// Immediate: early return from a definition. Not valid outside one.
static void w_exit() {
  if (!compiling) { forthAbort("exit only inside a definition"); return; }
  emit(OP_EXIT);
}

// Lists only what was defined after forthMarkCore(), i.e. the user's own words.
static void w_user() {
  int col = 0;
  for (int i = coreCount; i < dictCount; i++) {
    put(dict[i].name);
    putc_(' ');
    if (++col % 8 == 0) put("\r\n");
  }
  if (col % 8) put("\r\n");
  if (col == 0) put("(none)\r\n");
}

static const char *nameOf(int w) {
  return (w >= 0 && w < dictCount) ? dict[w].name : "???";
}

// Prints the threaded code of a definition. Branch targets are shown as raw
// code addresses — enough to read the structure back without a full recompiler.
static void w_see() {
  const char *name = nextToken();
  if (!name) { forthAbort("name expected"); return; }
  int w = find(name);
  if (w < 0) { forthAbort("undefined word"); return; }
  DictEntry &e = dict[w];

  if (e.flags & F_NATIVE) { portPrintf("%s  <native>%s\r\n", e.name, (e.flags & F_IMMEDIATE) ? " immediate" : ""); return; }
  if (e.flags & F_CONST)  { portPrintf("%s  <constant> ", e.name); putn(e.val); put("\r\n"); return; }
  if (e.flags & F_VAR)    { portPrintf("%s  <variable> @ ", e.name); putn(e.val); put("\r\n"); return; }

  portPrintf(": %s  ", e.name);
  uint16_t p = e.addr;
  while (p < codeHere) {
    cell x = code[p];
    if (x == OP_EXIT) { portPrintf("[%u] ;\r\n", (unsigned)p); return; }
    portPrintf("[%u] ", (unsigned)p);
    p++;
    switch (x) {
      case OP_LIT:     putn(code[p++]); putc_(' '); break;
      case OP_STR:     portPrintf(".\" %s\" ", (const char *)(uintptr_t)code[p++]); break;
      case OP_BRANCH:  portPrintf("branch->%u ", (unsigned)code[p++]); break;
      case OP_ZBRANCH: portPrintf("0branch->%u ", (unsigned)code[p++]); break;
      case OP_DO:      portPrintf("do(exit->%u) ", (unsigned)code[p++]); break;
      case OP_LOOP:    portPrintf("loop->%u ", (unsigned)code[p++]); break;
      case OP_LEAVE:   put("leave "); break;
      default:         put(nameOf((int)x)); putc_(' '); break;
    }
  }
  put("\r\n");
}

// Rewinds the dictionary and every pool to just before `name` was defined,
// taking everything defined after it — classic Forth forget semantics.
static void w_forget() {
  const char *name = nextToken();
  if (!name) { forthAbort("name expected"); return; }
  int w = find(name);
  if (w < 0) { forthAbort("undefined word"); return; }
  if (w < coreCount) { forthAbort("cannot forget a built-in word"); return; }
  DictEntry &e = dict[w];
  dictCount = w;
  codeHere = e.codeMark;
  nameHere = e.nameMark;
  dataHere = e.dataMark;
  strHere  = e.strMark;
  srcHere  = e.srcMark;
  srcLog[srcHere] = 0;
}

// Removes every user definition, leaving the built-in vocabulary intact.
static void w_wipe() {
  dictCount = coreCount;
  codeHere = coreCode;
  nameHere = coreName;
  dataHere = coreData;
  strHere  = coreStr;
  srcHere = 0;
  srcLog[0] = 0;
}

// Prints the stored source, one numbered line per definition.
static void w_list() {
  if (srcHere == 0) { put("(no saved definitions)\r\n"); return; }
  int n = 1;
  const char *p = srcLog;
  while (*p) {
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    portPrintf("%3d  ", n++);
    for (int i = 0; i < len; i++) putc_(p[i]);
    put("\r\n");
    if (!nl) break;
    p = nl + 1;
  }
  portPrintf("     %u/%d bytes\r\n", (unsigned)srcHere, SRC_LOG);
}

const char *forthSource() { srcLog[srcHere] = 0; return srcLog; }
int forthSourceLen() { return srcHere; }
void forthClearSource() { srcHere = 0; srcLog[0] = 0; }

void forthMarkCore() {
  coreCount = dictCount;
  coreCode = codeHere;
  coreName = nameHere;
  coreData = dataHere;
  coreStr = strHere;
}

static void logLine(const char *line) {
  int len = strlen(line);
  if (srcHere + len + 2 > SRC_LOG) { put("!! definition log full — not saved\r\n"); return; }
  memcpy(&srcLog[srcHere], line, len);
  srcHere += len;
  srcLog[srcHere++] = '\n';
  srcLog[srcHere] = 0;
}

static void w_comment()     { parseUntil(')'); }
static void w_lineComment() { while (*inPtr) inPtr++; }

static void w_dotQuote() {
  const char *s = parseUntil('"');
  if (compiling) {
    const char *stored = internName(s);
    if (!stored) return;
    emit(OP_STR);
    emit((cell)(uintptr_t)stored);
  } else {
    put(s);
  }
}

// ---------------------------------------------------------------------------
// Outer interpreter
// ---------------------------------------------------------------------------
static void rollbackDefinition() {
  if (!compiling) return;
  dictCount = defDict;
  codeHere = defCode;
  nameHere = defName;
  compiling = false;
}

void forthEval(const char *line) {
  const char *savedIn = inPtr;
  inPtr = line;
  aborting = false;

  const char *tok;
  while (!aborting && (tok = nextToken()) != NULL) {
    int w = find(tok);
    if (w >= 0) {
      if (compiling && !(dict[w].flags & F_IMMEDIATE)) emit(w);
      else execute(w);
      continue;
    }
    cell n;
    if (parseNumber(tok, &n)) {
      if (compiling) { emit(OP_LIT); emit(n); }
      else forthPush(n);
      continue;
    }
    put("?? ");
    put(tok);
    put("\r\n");
    aborting = true;
  }

  if (aborting) {
    rollbackDefinition();
    dsp = 0;
    rsp = 0;
    cfsp = 0;
    aborting = false;
  }
  inPtr = savedIn;
}

// A line that added to the dictionary is a definition, so it goes in the log.
// Lines that only computed something, or failed and rolled back, do not.
static void evalAndLog(const char *line) {
  int before = dictCount;
  forthEval(line);
  if (dictCount > before) logLine(line);
}

// Replays stored definitions and adopts them as the current log. Do not pass
// forthSource() here — it is the log itself.
void forthLoadSource(const char *text) {
  if (!text || text == srcLog) return;
  forthClearSource();
  char line[FORTH_LINE_MAX];
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    if (len > FORTH_LINE_MAX - 1) len = FORTH_LINE_MAX - 1;
    memcpy(line, p, len);
    line[len] = 0;
    if (len) evalAndLog(line);
    if (!nl) break;
    p = nl + 1;
  }
}

// ---------------------------------------------------------------------------
// Line-oriented REPL
// ---------------------------------------------------------------------------
static char lineBuf[FORTH_LINE_MAX];
static int lineLen = 0;

static void prompt() { put(compiling ? "... " : "ok> "); }

void forthPoll() {
  for (;;) {
    int c = portGetc();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      put("\r\n");
      lineBuf[lineLen] = 0;
      // A sink (file capture) gets first refusal on the line, blanks included.
      bool consumed = forthLineSink && forthLineSink(lineBuf);
      if (!consumed) {
        if (lineLen) evalAndLog(lineBuf);
        prompt();
      }
      lineLen = 0;
    } else if (c == 8 || c == 127) {
      if (lineLen > 0) { lineLen--; put("\b \b"); }
    } else if (c >= 32 && c < 127) {
      if (lineLen < FORTH_LINE_MAX - 1) { lineBuf[lineLen++] = (char)c; putc_((char)c); }
    }
  }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
struct NativeDef { const char *name; ForthFn fn; bool immediate; };

static const NativeDef natives[] = {
  // stack
  {"dup", w_dup, false}, {"?dup", w_qdup, false}, {"drop", w_drop, false},
  {"swap", w_swap, false}, {"over", w_over, false}, {"nip", w_nip, false},
  {"tuck", w_tuck, false}, {"rot", w_rot, false}, {"depth", w_depth, false},
  {"clear", w_clear, false},
  // arithmetic / logic
  {"+", w_add, false}, {"-", w_sub, false}, {"*", w_mul, false},
  {"/", w_div, false}, {"mod", w_mod, false}, {"negate", w_neg, false},
  {"abs", w_abs, false}, {"min", w_min, false}, {"max", w_max, false},
  {"and", w_and, false}, {"or", w_or, false}, {"xor", w_xor, false},
  {"invert", w_inv, false}, {"lshift", w_lsh, false}, {"rshift", w_rsh, false},
  {"1+", w_1add, false}, {"1-", w_1sub, false},
  {"=", w_eq, false}, {"<>", w_ne, false}, {"<", w_lt, false}, {">", w_gt, false},
  {"<=", w_le, false}, {">=", w_ge, false}, {"0=", w_zeq, false}, {"0<", w_zlt, false},
  // memory (raw pointers: works on data space and on peripheral registers alike)
  {"@", w_fetch, false}, {"!", w_store, false}, {"c@", w_cfetch, false},
  {"c!", w_cstore, false}, {"+!", w_plusStore, false},
  {"bis!", w_bis, false}, {"bic!", w_bic, false},
  {"cells", w_cells, false}, {"cell+", w_cellPlus, false},
  // io
  {".", w_dot, false}, {"u.", w_uDot, false}, {".s", w_dotS, false},
  {"?", w_question, false}, {"emit", w_emit, false}, {"cr", w_cr, false},
  {"space", w_space, false}, {"hex", w_hex, false}, {"decimal", w_dec, false},
  {"words", w_words, false}, {"free", w_free, false},
  // vocabulary
  {"user", w_user, false}, {"see", w_see, false}, {"forget", w_forget, false},
  {"wipe", w_wipe, false}, {"list", w_list, false}, {"exit", w_exit, true},
  // strings
  {"s\"", w_sQuote, true}, {"type", w_type, false}, {"string", w_string, false},
  {"s!", w_sStore, false}, {"s+", w_sAppend, false}, {"sc+", w_sAppendChar, false},
  {"s#", w_sAppendNum, false}, {"s@", w_sFetch, false}, {"slen", w_sLen, false},
  {"sroom", w_sRoom, false}, {"sclear", w_sClear, false}, {"s.", w_sPrint, false},
  {"s=", w_sEqual, false}, {"/string", w_slashString, false},
  // return stack
  {">r", w_toR, false}, {"r>", w_rFrom, false}, {"r@", w_rAt, false},
  {"i", w_i, false}, {"j", w_j, false},
  // defining
  {":", w_colon, true}, {";", w_semicolon, true}, {"immediate", w_immediate, false},
  {"variable", w_variable, false}, {"array", w_array, false},
  {"constant", w_constant, false}, {"'", w_tick, true}, {"execute", w_execute, false},
  // control flow
  {"if", w_if, true}, {"else", w_else, true}, {"then", w_then, true},
  {"begin", w_begin, true}, {"again", w_again, true}, {"until", w_until, true},
  {"while", w_while, true}, {"repeat", w_repeat, true},
  {"do", w_do, true}, {"loop", w_loop, true}, {"leave", w_leave, true},
  // lexical
  {"(", w_comment, true}, {"\\", w_lineComment, true}, {".\"", w_dotQuote, true},
};

void forthInit() {
  dictCount = 0;
  codeHere = 0;
  nameHere = 0;
  dataHere = 0;
  strHere = 0;
  srcHere = 0;
  srcLog[0] = 0;
  coreCount = 0;
  coreCode = coreName = coreData = coreStr = 0;
  transientIdx = 0;
  dsp = rsp = cfsp = 0;
  compiling = false;
  aborting = false;
  numBase = 10;

  for (unsigned i = 0; i < sizeof(natives) / sizeof(natives[0]); i++)
    forthAddWord(natives[i].name, natives[i].fn, natives[i].immediate);

  forthAddConstant("true", -1);
  forthAddConstant("false", 0);
}
