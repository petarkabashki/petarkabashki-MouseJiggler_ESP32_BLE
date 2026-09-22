"""Writes data/symbols.txt after every link, for the Forth FFI's `sym` word.

The table has to describe the image it is generated from, and it cannot live
inside that image without changing it, so it goes on the filesystem instead:
build, then `pio run -t uploadfs`. The firmware detects a table left over from
an earlier build by checking one sentinel address it knows at compile time.

Only global function symbols are listed. Locals are omitted because there are
several thousand of them and they are rarely what anyone wants to call.
"""

import os
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)


SENTINEL = "esp_restart"


def nm_tool():
    cc = env.subst("$CC")
    for suffix in ("-gcc", "-clang", "-cc"):
        if cc.endswith(suffix):
            return cc[: -len(suffix)] + "-nm"
    return "nm"


def generate(source, target, env):
    elf = str(target[0])
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
    out = os.path.join(out_dir, "symbols.txt")

    try:
        raw = subprocess.check_output(
            [nm_tool(), "--defined-only", elf], universal_newlines=True
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        print("gen_symbols: skipped (%s)" % exc)
        return

    syms = []
    for line in raw.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        addr, kind, name = parts
        # T is a global function, W a weak one (the overridable IDF hooks).
        if kind not in ("T", "W"):
            continue
        syms.append((name, addr))

    syms.sort()

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    with open(out, "w") as fh:
        for name, addr in syms:
            fh.write("%s %s\n" % (addr, name))

    have_sentinel = any(name == SENTINEL for name, _ in syms)
    print(
        "gen_symbols: %d symbols -> data/symbols.txt (%.1f KB)%s"
        % (
            len(syms),
            os.path.getsize(out) / 1024.0,
            "" if have_sentinel else "  [WARNING: no %s, staleness check disabled]" % SENTINEL,
        )
    )
    print("gen_symbols: run `pio run -t uploadfs` to put it on the device")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", generate)  # noqa: F821
