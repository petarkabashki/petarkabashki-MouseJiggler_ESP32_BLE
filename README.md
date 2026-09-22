# 🖱️ ESP32-C3 BLE Mouse Jiggler v1.1

A simple, feature-rich Mouse Jiggler using an ESP32-C3 SuperMini and BLE to keep your system awake. This version adds status LEDs, mode memory, and more intuitive controls.

## 🚀 What's New in v1.1

- 💡 **LED Status Indicator**: The onboard LED now blinks with a unique pattern for each mode, so you always know what it's doing.
- 🧠 **Mode Memory**: Remembers your last-used mode, even after being unplugged.
- ✍️ **Natural Scribble Mode**: The ZIGZAG mode now simulates a more human-like scribble.
- 🛠️ **Intuitive Controls**: The button logic has been fixed. A single click now toggles the jiggler on (SLOW mode) and off.

## ✨ Features

- Works wirelessly via BLE.
- Single-button control for all modes.
- Visual feedback via the onboard LED.
- Multiple Jiggle Modes:
  - 🟢 **SLOW Mode**: Moves mouse every 60 seconds (wide range).
  - ⚡ **FAST Mode**: Moves mouse every second (small range).
  - 🌀 **ZIGZAG Mode**: Constant, natural scribble-like motion.
- Stability enhancements including a watchdog timer and auto-reconnect logic.

## 🛠️ Hardware Requirements

- ESP32-C3 SuperMini board **or** an ESP32-C3 "egg" board with a built-in 0.42" OLED
- USB Type-C Data Cable
- PC/Laptop with Bluetooth
- Optional: Breadboard/Case for enclosure

### 🥚 0.42" OLED "egg" board

Cheap ESP32-C3 boards with a tiny built-in 0.42" SSD1306-compatible OLED (72x40px)
are also supported, using the `esp32-c3-042oled` PlatformIO environment:

```
pio run -e esp32-c3-042oled -t upload
```

Pinout used on these boards: OLED SDA=GPIO5, SCL=GPIO6, onboard LED=GPIO8,
BOOT button=GPIO9 (same button/LED pins as the SuperMini, so no code changes
are needed beyond selecting this environment). The screen shows the current
jiggle mode and BLE connection status, and updates whenever either changes.

## 📦 Setup Instructions (PlatformIO)

### 1. 🔧 Install VS Code & PlatformIO

- Download & install VS Code
- Install the PlatformIO IDE extension

### 2. 🆕 Create New Project

- Open PlatformIO Home (Alien icon).
- Click **New Project**.
- Fill in the details:
  - **Name**: MouseJiggler
  - **Board**: Espressif ESP32-C3 Dev Module
  - **Framework**: Arduino
- Click **Finish**.

### 3. 📄 Add Code & Libraries

- Paste the main code into `src/main.cpp`.
- The **ESP32-BLE-Mouse** library ships with this repository under `lib/`, extended with keyboard reports; there is nothing to add to `lib_deps`.

### 4️⃣ Configure `platformio.ini`

```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200
board_build.partitions = huge_app.csv
build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=1
```

The `ESP32-BLE-Mouse` library is **vendored in [lib/](lib/ESP32-BLE-Mouse/)**, not pulled from
`lib_deps`: it has been extended into a combo mouse **and keyboard** HID
device. WiFi, BLE and the interpreter together outgrow the default 1.3 MB app
slot, hence `huge_app.csv`.

## 🎮 Button Controls (BOOT Button)

The controls are now simpler and more intuitive. The LED provides feedback for each mode.

| Press Count | Action                                 | LED Indicator |
|-------------|----------------------------------------|----------------|
| 1 Press     | Toggles between OFF and SLOW mode.     | Off / Slow Blink |
| 2 Presses   | Activates FAST mode.                   | Medium Blink     |
| 3 Presses   | Activates ZIGZAG (Scribble) mode.      | Fast Blink       |

## ⌨️ Serial Terminal (embedded Forth)

Open the serial monitor at 115200 baud and you get a prompt. The device runs a
small Forth interpreter, so you can drive the mouse and poke at the hardware
interactively — no reflash needed to try something.

```
pio device monitor -e esp32-c3-devkitm-1
```

```forth
ok> help            \ list the device words
ok> words           \ list everything, core words included
ok> conn? .         \ -1 when a BLE host is connected
ok> 20 0 move       \ move 20px right
ok> fast mode! save \ switch mode and persist it
ok> left click
```

Define your own words; they behave like any built-in:

```forth
ok> : wiggle 20 0 do 5 0 move 40 ms -5 0 move 40 ms loop ;
ok> wiggle
ok> : human 10 40 random 10 40 random move ;
ok> ' human task!  3000 every     \ run it every 3 s; `task-off` stops it
```

### Jiggler state

`.state` prints everything at once. The jiggle parameters are live variables,
not compiled-in constants, so you can tune the behaviour and keep it:

```forth
ok> .state
ok> 2000 fast-ms !  8 fast-px !   \ retune FAST while it runs
ok> fast mode!  save              \ persist mode + all parameters
ok> defaults save                 \ back to the built-in values
```

| Variable | Meaning |
|---|---|
| `slow-ms` / `slow-px` | SLOW interval and movement range |
| `fast-ms` / `fast-px` | FAST interval and range |
| `zig-ms` / `zig-px` / `zig-phase` | ZIGZAG interval, range, steps per direction |
| `keepalive-ms` | idle BLE keep-alive period (0 disables) |
| `zig-step` | the scribble's step counter, driving the drift quadrant |

### Strings

A string is `( addr len )` on the stack. `s" ..."` makes one, `type` prints it.
Named buffers hold text you build up; every write truncates at the buffer's
capacity instead of failing:

```forth
ok> s" hello" type cr
ok> 32 string line
ok> s" mode: " line s!   mode$ line s+   line s. cr
ok> line sclear  s" px=" line s!  fast-px @ line s#  line s.
```

`s!` set · `s+` append · `sc+` append char · `s#` append number · `s@` fetch as
`( addr len )` · `s.` print · `slen` · `sroom` · `sclear` · `s=` compare ·
`/string` drop leading chars.

### Screen

Display words exist on every build — they are no-ops where there is no panel,
so the same script runs on both boards. `screen?` tells you which you have.
`false screen-auto` stops the built-in status layout redrawing so a script can
own the display:

```forth
ok> s" jiggling" status!          \ caption on the stock layout
ok> false screen-auto
ok> : hud cls  1 font  mode$ 2 12 puts  heap 2 24 putn  show ;
ok> ' hud task!  500 every
ok> true screen-auto              \ hand it back
```

`cls` · `show` · `( a u x y ) puts` · `( n x y ) putn` · `n font` (0 tiny,
1 normal, 2 bold) · `x y w hline` · `x y w h box` / `frame` · `x y pixel` ·
`n contrast`.

`@` and `!` take real addresses, so peripheral registers and any exported C++
symbol are reachable without writing a binding for them:

```forth
ok> hex 60004004 @ .   \ GPIO_OUT_REG
```

### The jiggler is itself written in Forth

There is no C++ jiggle loop any more. `loop()` calls one Forth word, `tick`,
which is defined in a boot script in [src/main.cpp](src/main.cpp) and can be read back on the
device with `see`:

```forth
ok> see tick
ok> see jiggle
ok> see zig-move
```

`tick` is looked up by name on every pass, so redefining it changes what the
device does on the spot:

```forth
ok> : tick jiggler-tick ;          \ keep jiggling, drop the keep-alive
ok> : tick ;                       \ stop entirely
ok> : tick jiggler-tick keepalive s" running" status! ;
ok> forget tick                    \ back to the built-in
```

The parts below `tick` are resolved when the caller is compiled, as in any
Forth: redefining `jiggle` does not change the existing `jiggler-tick`. To
swap a piece out, redefine it and then redefine `tick` (or `jiggler-tick`)
so the new version is the one that gets compiled in.

The built-in definitions are part of the core, so `wipe` and `forget` will not
remove them; redefining a name shadows it, and forgetting your redefinition
uncovers the original.

### Keyboard

The bundled BLE library is a combo HID device: it reports as a mouse *and* a
keyboard on one connection. Modifiers come first, so a line reads like the
chord you are pressing:

```forth
ok> 0 k-enter key                  \ tap Enter
ok> gui k-space key                \ GUI+Space
ok> ctrl shift or k-esc key        \ combine modifiers with  or
ok> s" hello world" keys           \ type a string
ok> gui 0 key+  200 ms  key-clear  \ hold GUI, then let go
```

`key` tap · `key+` hold · `key-` release one · `key-clear` release all ·
`keyc` type one character · `keys` type a string. Constants: `ctrl shift alt
gui`, `k-enter k-esc k-bs k-tab k-space k-del k-home k-end k-pgup k-pgdn
k-up k-down k-left k-right k-f1`.

> **Re-pairing.** Adding the keyboard changes the HID report descriptor, so
> the device looks different to hosts that had already paired with it. Remove
> the old pairing on the host (and `ble-unbond` on the device) the first time.

### BLE and WiFi

```forth
ok> ble-mac type cr   ble-name type cr
ok> ble-drop                       \ hang up; advertising resumes
ok> ble-bonds                      \ list paired hosts
ok> ble-unbond                     \ forget them all
```

```forth
ok> wifi-scan
ok> s" my-network" ssid!  s" secret" pass!  wifi-connect
ok> wifi? . wifi-ip type cr wifi-rssi .
ok> wifi-save                      \ remember the credentials
ok> wifi-off
ok> s" jiggler" ssid!  s" 12345678" pass!  wifi-ap
```

> WiFi credentials are stored in NVS as plaintext, exactly as `Preferences`
> keeps them. Anyone with physical access to the board can read them back.

### Files and `boot.fs`

`huge_app.csv` leaves an 896 KB data partition, which LittleFS takes over. The
device keeps Forth source there and runs `/boot.fs` at every startup, before
the prompt appears:

```forth
ok> ls                        \ names and sizes;  df  for space
ok> s" boot.fs" edit          \ capture typed lines into a file
... 2000 fast-ms !
... : wiggle 20 0 do 5 0 move 40 ms -5 0 move 40 ms loop ;
... ;;
wrote /boot.fs (78 bytes)
ok> s" boot.fs" cat
ok> s" hud.fs" include        \ run a file now; usable inside boot.fs too
ok> s" backup.fs" save-to     \ dump this session's definitions to a file
ok> s" old.fs" rm
```

Definitions may span several lines in a file, exactly as you would write them
by hand. `\` comments work per line; keep `( ... )` comments on one line.

**Hold the BOOT button while powering up** and the device comes back bare —
`boot.fs` and the NVS-saved words are both skipped, and it says so. That is
the way out of a boot script that breaks the prompt.

Files can also be pushed from the host, which is easier for anything long:

```
pio run -t uploadfs           \ uploads everything in data/ — see data/boot.fs
```

Because `loop()` calls nothing but the Forth word `tick`, a `boot.fs` that
redefines `tick` has replaced the firmware's main loop without a reflash.

> **Why not a USB drive?** On an ESP32-**S2/S3** this would mount as mass
> storage like MicroPython's. The **C3 cannot**: its USB is a fixed-function
> Serial/JTAG block, not an OTG controller (`soc_caps.h` defines
> `SOC_USB_SERIAL_JTAG_SUPPORTED` and no `SOC_USB_OTG_SUPPORTED`, and
> TinyUSB's `msc` class ships only for the S2/S3). No amount of firmware
> changes that, so files move over this terminal or `uploadfs` instead.

### Saving and inspecting your vocabulary

Every line that adds a definition is kept as source text, so the device can
show you what you have taught it and hand it back after a reboot:

```forth
ok> user                 \ just your words, not the built-ins
ok> list                 \ the source of everything you have defined
ok> see wiggle           \ decompile one word
ok> forget wiggle        \ remove it and everything defined after it
ok> wipe                 \ back to the factory vocabulary
ok> save-words           \ persist to NVS — replayed automatically at boot
ok> load-words           \ reload it now;  drop-words  erases the saved copy
```

`forget` refuses to cross into the built-ins, so the jiggler and the device
words are always there to fall back on.

### Calling compiled code directly (FFI)

Every word below is a C++ function someone chose to expose. The FFI is the
other end of that trade: given an address and an argument count, `call` puts
the data stack into the platform's calling convention and jumps. Anything
linked into the image is reachable without writing a binding for it.

```forth
ok> s" esp_random" sym constant &rnd    \ resolve once, keep the address
ok> &rnd 0call .                        \ 1259786311
ok> 250 s" vTaskDelay" sym 1call drop   \ a void function still returns junk
ok> s" gpio_" syms                      \ everything with "gpio_" in the name
```

| word | stack | |
|---|---|---|
| `sym` | `( addr len -- fn \| 0 )` | look a name up in the symbol table |
| `syms` | `( addr len -- )` | print every symbol containing that substring |
| `0call`..`4call` | `( args fn -- result )` | call with a fixed argument count |
| `call` | `( args fn n -- result )` | call with `n` arguments, 0..8 |
| `cstr` | `( addr len -- zaddr )` | NUL-terminate a string so C can take it |
| `zcount` | `( zaddr -- addr len )` | and the way back, for a returned `char *` |

Notes worth knowing before you point it at something:

- **Arguments are pushed in source order.** `1 2 3 fn 3call` calls `fn(1,2,3)`.
- **`call` always leaves a result.** A `void` function leaves the return
  register untouched, so what you get is junk — `drop` it.
- **Eight arguments, integers and pointers only.** That is what fits the
  RISC-V register convention a plain function-pointer cast can reach; floats
  and structs by value need a real shim.
- **`cstr` has four rotating buffers**, so one call can take several strings.
  The fifth overwrites the first.
- **A bad address is refused, not called.** `call` checks the address against
  the C3's executable ranges first, because jumping into data reboots the chip.

The symbol table is not in the firmware — it describes the firmware, so it
cannot be inside it. `tools/gen_symbols.py` writes `data/symbols.txt` after
every link (about 6,300 names, ~210 KB), and it reaches the device with the
filesystem:

```bash
pio run -e esp32-c3-devkitm-1        # writes data/symbols.txt
pio run -e esp32-c3-devkitm-1 -t uploadfs
```

Build the environment you are actually flashing. A table left over from a
different build lists addresses that have all moved, and calling one of those
is an instant crash — so `sym` checks the table against one address the
firmware knows at compile time, and refuses everything if they disagree:

```
symbol table is stale (built from a different image) — re-run `pio run -t uploadfs`
```

Lookups scan the file, which takes about a second. Resolve into a `constant`
once, as above, rather than calling `sym` in a loop.

### Adding your own words

The FFI reaches anything, but a word you will type often deserves a name.
The bridge is one small function plus one registration line in
[src/main.cpp](src/main.cpp). Arguments come off the data stack, results go back on it:

```cpp
static void fw_move() { cell dy = forthPop(), dx = forthPop(); bleMouse.move(dx, dy); }
...
forthAddWord("move", fw_move);
forthAddConstant("led-pin", LED_PIN);
forthAddVariable("fast-ms", &cfg.fastInterval);  // exposes a C++ variable directly
```

Strings cross the boundary with two helpers — no marshalling layer:

```cpp
static void fw_statusStore() { forthPopString(statusLine, sizeof(statusLine)); }  // ( addr len -- )
static void fw_modeName()    { forthPushString(modeName(jiggleMode)); }           // ( -- addr len )
```

The interpreter itself is [src/forth.cpp](src/forth.cpp) / [src/forth.h](src/forth.h): fully static
storage (no `malloc`), non-blocking line reader, and a `forthYield` hook wired
to the watchdog so long scripts don't reset the board.

### A Forth-first sibling, without Arduino

The interpreter does not depend on Arduino. Everything it needs from the
platform is [src/port.h](src/port.h) — a console, a clock, a yield, a
filesystem mount and a handful of system calls — and
[src/port_arduino.cpp](src/port_arduino.cpp) is this build's side of it.

A second project, **`../forthkit`**, supplies the other side: the same
`forth.cpp`, `forth_ffi.cpp` and `forth_files.cpp` on bare ESP-IDF, with no
Arduino core and no Arduino libraries. There the Forth system is the kernel and
the jiggler is one application under `apps/`; it fits in 777 KB of flash and
61 KB of RAM against this build's 1,488 KB and 90 KB, and has the BLE HID mouse
and keyboard, the filesystem, `boot.fs` and the FFI — but **not** WiFi or the
OLED, which is why this build is still the one to flash if you want either.

That project is where the Forth-first work continues. This one stays the
Arduino variant.

## 💡 Notes

- Make sure you're using a proper USB data cable (not charge-only).
- The BLE Mouse will appear as: `"Wireless Jiggler"`.
- First-time pairing may require confirmation on your OS.

## 🛡️ License

This project is licensed under the MIT License.

## 🙌 Credits

- **ESP32-BLE-Mouse** by **T-vK**
