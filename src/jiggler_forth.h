// The jiggler itself, written in Forth. Shared verbatim by both builds: the
// Arduino one in main.cpp and the ESP-IDF one in main_idf.cpp evaluate this
// before forthMarkCore(), so it counts as part of the core vocabulary.
//
// Everything it needs from the device is a handful of words — move, conn?,
// mode@, millis, random and the timing variables — which is why the same text
// works on both.
#pragma once

// ---------------------------------------------------------------------------
// The jiggler, in Forth. This runs at boot before forthMarkCore(), so it
// counts as part of the core: `words` shows it, `see` prints it, and `wipe`
// leaves it alone. Redefine any of it from the terminal — the newest
// definition wins, and `forget` takes you back.
// ---------------------------------------------------------------------------
static const char *jigglerForth[] = {
  "variable last-jiggle",
  "variable last-ka",
  // Random helpers over a range r: symmetric, positive-only, negative-only.
  ": rnd~ ( r -- n ) dup negate swap 1+ random ;",
  ": rnd+ ( r -- n ) 1+ 0 swap random ;",
  ": rnd- ( r -- n ) negate 1 random ;",
  ": slow-move slow-px @ rnd~ slow-px @ rnd~ move ;",
  ": fast-move fast-px @ rnd~ fast-px @ rnd~ move ;",
  // The scribble drifts into one quadrant for `zig-phase` steps at a time.
  ": zig-bias zig-step @ zig-phase @ 1 max / 3 and ;",
  ": zig0 dup rnd+ swap rnd+ move ;",
  ": zig1 dup rnd- swap rnd+ move ;",
  ": zig2 dup rnd- swap rnd- move ;",
  ": zig3 dup rnd+ swap rnd- move ;",
  ": zig-move zig-px @ zig-bias"
  "  dup 0= if drop zig0 else dup 1 = if drop zig1"
  "  else 2 = if zig2 else zig3 then then then"
  "  1 zig-step +! ;",
  ": jiggle ( -- ) mode@"
  "  dup 1 = if drop slow-move exit then"
  "  dup 2 = if drop fast-move exit then"
  "  3 = if zig-move then ;",
  ": jiggle-ms ( -- ms ) mode@"
  "  dup 1 = if drop slow-ms @ exit then"
  "  dup 2 = if drop fast-ms @ exit then"
  "  3 = if zig-ms @ else 0 then ;",
  ": due? ( addr interval -- f ) swap @ millis swap - swap >= ;",
  ": jiggler-tick"
  "  conn? 0= if exit then"
  "  jiggle-ms dup 0= if drop exit then"
  "  last-jiggle swap due? if jiggle millis last-jiggle ! then ;",
  ": keepalive"
  "  conn? 0= if exit then"
  "  keepalive-ms @ dup 0= if drop exit then"
  "  last-ka swap due? if 0 0 move millis last-ka ! then ;",
  // Called from loop(); redefine it to change what the device does per tick.
  ": tick jiggler-tick keepalive ;",
};
