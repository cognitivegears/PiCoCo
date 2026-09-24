# PiCoCo manager (CoCo side)

Build: `make` (needs CMOC, LWTOOLS, ToolShed `decb`). Host tests: `make test`.

## Toolchain

- **LWTOOLS** (`lwasm`, `lwlink`, `lwar`) and **XRoar** via `brew install lwtools xroar`.
- **CMOC** (0.1.100) built from source from `http://sarrazip.com/dev/cmoc.html`
  and installed to `/opt/homebrew` (`./configure --prefix=/opt/homebrew && make
  && make install`). CMOC's `configure` detects `lwasm`/`lwlink`/`lwar`, so
  LWTOOLS must already be on `PATH`. The tarball's `doc/cmoc-manual.markdown`
  (checked against `--org`, `asm {}`, `inkey()` syntax in later tasks) is kept
  at `/opt/homebrew/share/doc/cmoc/cmoc-manual.markdown`.
- **ToolShed** (`decb`, for building `.DSK` images) built from
  `https://github.com/nitros9project/toolshed.git` via `make -C build/unix`,
  then `decb` and `os9` copied by hand into `/opt/homebrew/bin` (no `sudo`).
  The `cocofuse` and `ToolShed.html` targets fail in that same `make` run
  (missing `fuse_t/fuse.h` and the `markdown` command respectively) — both are
  unrelated to `decb`/`os9` and safe to ignore; the unit tests
  (`librbftest`, `libdecbtest`, `libcecbtest`, `libtoolshedtest`,
  `os9commandtest`, `decbcommandtest`) all pass regardless.

## ROMs

XRoar needs `bas13.rom`, `extbas11.rom` (CoCo 2) and `coco3.rom` (CoCo 3) in
`~/.xroar/roms/` — these are copyrighted and not redistributed with this repo.
HDB-DOS's becker-port ROMs (`hdbdw3bck.rom` for CoCo 2, `hdbdw3bc3.rom` for
CoCo 3) live in `firmware/roms/` (gitignored) and also need copying into
`~/.xroar/roms/`.

## Emulate

```
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host
mkdir -p /tmp/picoco-pcdisk && cp coco/PICOCO.DSK /tmp/picoco-pcdisk/
build-host/picoco-host --dir /tmp/picoco-pcdisk --mount 0=PICOCO.DSK &
xroar -machine coco2bus -becker -rompath ~/.xroar/roms
```

At the `OK` prompt type `RUN"PICOCO"`. Expect `PICOCO HELLO` then `OK`.

For the CoCo 3 profile, add `-cart-rom hdbdw3bc3` (see below for why):

```
xroar -machine coco3 -cart-rom hdbdw3bc3 -becker -rompath ~/.xroar/roms
```

Same result: `RUN"PICOCO"` prints `PICOCO HELLO` then `OK`. (If a local
sandbox or firewall blocks `picoco-host` or XRoar's becker port from
listening/connecting on localhost, allow local TCP for this session or run
outside it — the exact mechanism depends on your environment.)

Notes on the exact flags, confirmed against this XRoar build (1.12.1):

- There is no cart profile literally named `hdbdos`. The predefined cart
  profile that does what we want is `becker` (`RS-DOS with becker port`,
  `-cart-type rsdos`, ROM list `rsdos_becker` = `hdbdw3bck`).
- **Neither machine needs an explicit `-cart becker`.** Both `coco2bus` and
  `coco3` already default to `machine-cart becker` at runtime (check with
  `xroar -machine coco3 -config-print | grep machine-cart`); the static
  `-machine help`/`-config-print-all` listing doesn't show this because it's
  a runtime default, not a property of the named machine profile itself.
  `-becker` (the boolean "prefer a becker-enabled DOS") is still worth
  passing explicitly for clarity, but isn't strictly required either.
- **CoCo 3 needs its ROM overridden.** The `becker` cart's ROM list
  (`rsdos_becker` = `hdbdw3bck`) is the *CoCo 2* HDB-DOS build regardless of
  which machine you select — nothing switches it automatically for `coco3`.
  Booting `-machine coco3` without an override runs the CoCo 2 ROM under
  CoCo 3 hardware: it happens to still work (boots, becker port answers,
  `RUN"PICOCO"` succeeds), but the banner misleadingly reads "HDB-DOS 1.5
  BECKER COCO 2" and "COLOR BASIC 1.1" on CoCo 3 hardware. Passing
  `-cart-rom hdbdw3bc3` (verified via `xroar -help` → `-cart-rom NAME`, "ROM
  image to load ($C000-)") overrides just the ROM image on whichever cart is
  currently selected (the auto-selected `becker` cart, no explicit `-cart`
  needed first) and correctly boots "COLOR BASIC 2.1" / "HDB-DOS 1.5 BECKER
  COCO 3".
- XRoar's becker port defaults to `127.0.0.1:65504` (`-becker-ip`,
  `-becker-port`), which matches `picoco-host`'s default port — no flags
  needed on either side for the default setup.
- `xroar -machine help` machine names used here: `coco2bus` (Tandy CoCo 2B,
  NTSC, T1 — the CoCo 2 profile) and `coco3` (Tandy CoCo 3).

## Headless XRoar recipe (for scripted/automated verification)

Later tasks need to drive the CoCo and read back the screen without a human
watching a window. Two pieces make that possible:

1. **Keyboard input at startup**: `-type STRING` intercepts ROM keyboard
   calls and types `STRING` into BASIC once it's ready to accept input — no
   window or focus needed. Use `\r` for ENTER, e.g.:
   ```
   xroar ... -type 'RUN"PICOCO"\r'
   ```
   This is a *startup* option only — there is no live "inject these keys into
   the already-running emulator" CLI command. XRoar's own escape table (from
   its texinfo manual, "Escape sequences") covers more than ENTER: `\b`
   (BS/LEFT), `\t` (HT/RIGHT), `\n` (LF/DOWN), `\r` (CR/ENTER), `\f`
   (FF/CLEAR), `\e` (ESC, mapped to BREAK on CoCo/Dragon), `\xHH` / `\uHHHH`
   for an arbitrary byte/codepoint. There's no dedicated UP escape in that
   table; on real CoCo BASIC the up-arrow key is the same code as `^`
   (ASCII 0x5E), so a literal `^` in the `-type` string should produce it —
   not independently verified in this session, sourced from the escape table
   and CoCo keyboard-code convention only. Since `-type` feeds ASCII
   characters (not raw key-down/up events), SHIFT+letter, digits, and
   punctuation just need the right character/case in the string; there's no
   separate "hold shift" step.

   **We could not find a way to send more keystrokes to an already-running
   instance**, including via the GDB stub: this XRoar's GDB target
   (`-gdb`/`-gdb-port`, see its manual's "GDB target" section) implements
   only the standard read-memory/registers/continue/interrupt subset used
   below, with no monitor/`qRcmd`-style custom command and no documented way
   to feed the keyboard through it.

   **Important discovered limitation**: once `xrscreen.py` (or anything
   using the GDB stub) has interrupted the CPU even once, any `-type` text
   still queued past that point reliably never gets typed — confirmed by
   comparing an interrupted run (stuck forever, even given 60+ real seconds)
   against the identical `-type` string left completely undisturbed (finishes
   normally in well under 20s). The safe pattern is: put **everything** the
   session needs typed into the one `-type` string given at launch (e.g. a
   whole numbered BASIC program plus `RUN`, since *storing* program lines
   takes no CPU time worth mentioning), and only start polling with
   `xrscreen.py` once nothing more needs to be typed — from then on you're
   just observing a program that runs and updates the screen on its own,
   which tolerates being repeatedly halted and resumed just fine.

2. **Headless video/audio + screen capture**: `-ui null -ao null` runs XRoar
   with no window and no audio backend (confirmed working on this build,
   1.12.1 macOS/SDL2 build) — no dummy display needed. Combined with `-gdb`
   (default port 65520), `coco/tools/xrscreen.py` (stdlib-only Python 3)
   connects to the GDB remote stub, reads $0400-$05FF (the 32x16 text screen)
   with a `$m400,200#xx` packet, and prints the decoded screen as 16 lines of
   32 characters.

   VDG decode: for each screen byte `b`, `c = b & 0x3F`; the character is
   `chr(c + 0x40)` if `c < 0x20` else `chr(c)`. Bit 6 clear means inverse
   video, marked here by lowercasing the character. Bit 7 set means
   semigraphics, printed as `#`. The blinking text cursor renders as a
   semigraphics block, so a trailing `#` on the last non-blank line is normal
   and just the cursor.

   **One `xrscreen.py` session per XRoar instance — this is not optional.**
   XRoar's GDB stub only tolerates one client connection per run: closing a
   connection (even a clean one that resumed the CPU first with `c`) and then
   opening a second, separate connection from a fresh process reliably hangs
   — the TCP connect succeeds but no reply ever arrives for the first command
   sent. So `xrscreen.py` does all its work — one screenshot, or polling for
   several strings in turn — on a single connection held open for the whole
   invocation, and every exit path (success, timeout, or error) resumes the
   CPU (`c`) before closing that one connection, leaving the emulator running
   for whatever comes next. Never invoke `xrscreen.py` a second time against
   the same still-running XRoar instance; if you need to watch for more than
   one thing, pass `--wait-for` more than once in the same invocation. Also
   note that while `xrscreen.py` is talking to the GDB stub the emulated CPU
   is halted (frozen, including its DriveWire traffic to `picoco-host`) —
   each poll cycle resumes it before sleeping, so it's only paused for brief
   moments, not for the whole invocation.

   Usage:
   ```
   python3 coco/tools/xrscreen.py [--host HOST] [--port PORT]
   python3 coco/tools/xrscreen.py --wait-for TEXT [--wait-for TEXT ...]
                                   [--timeout SECONDS] [--interval SECONDS]
   python3 coco/tools/xrscreen.py --selftest
   ```
   With no `--wait-for`, it connects, takes one screenshot, resumes, and
   prints it — defaults are `127.0.0.1:65520`. With one or more `--wait-for
   TEXT`, it polls (read / continue / sleep / interrupt / read again, all on
   the one connection) until each `TEXT` appears on screen in turn, printing
   the screen at each match, letting the machine keep running between
   matches; exits 0 if every target was found, 1 if any timed out (printing
   the last screen seen for that target first). `--connect-timeout` (default
   5s) retries the initial connect, since a fresh XRoar instance sometimes
   isn't listening on its GDB port for a couple of seconds after launch.
   `--selftest` runs internal checksum/decode asserts with no XRoar
   connection needed — useful as a quick sanity check after editing the
   script.

   Full worked example (CoCo 2, after `picoco-host` is up):
   ```
   xroar -machine coco2bus -becker -rompath ~/.xroar/roms \
         -ui null -ao null -gdb -gdb-port 65520 -type 'RUN"PICOCO"\r' &
   sleep 3
   python3 coco/tools/xrscreen.py
   ```
   Output:
   ```
   DISK EXTENDED COLOR BASIC 1.1
   COPYRIGHT (C) 1982 BY TANDY
   UNDER LICENSE FROM MICROSOFT

   HDB-DOS 1.5 BECKER COCO 2

   OK
   RUN"PICOCO"
   PICOCO HELLO
   OK
   #
   ```

   Worked example of `--wait-for`, watching a self-contained BASIC program
   (typed and `RUN` all in one `-type` string, per the limitation above) run
   two loops and print a distinct, non-literal marker after each — the final
   loop variable value (`PRINT I` after `FOR I=1 TO 2000`, so it prints
   `2001`, a value that doesn't appear anywhere in the source text, unlike a
   literal you `PRINT` yourself):
   ```
   xroar -machine coco2bus -becker -rompath ~/.xroar/roms \
         -ui null -ao null -gdb -gdb-port 65520 \
         -type '10 CLS\r20 FOR I=1 TO 2000:NEXT I\r30 PRINT I\r40 FOR I=1 TO 3000:NEXT I\r50 PRINT I\rRUN\r' &
   sleep 2
   python3 coco/tools/xrscreen.py --wait-for 2001 --wait-for 3001 --timeout 30 --interval 1
   ```
   Output (abridged to the two "found" screens):
   ```
   --- [1/2] wait-for '2001': found ---
    2001
   --- [2/2] wait-for '3001': found ---
    2001
    3001
   OK
   #
   ```
   This demonstrates polling more than once in a single session while the
   machine keeps running unattended, and the screen visibly changing between
   waits. (An earlier attempt at this used two separate top-level commands,
   e.g. `CLS:FOR...\rCLS:FOR...\r`, and hit exactly the limitation described
   above: the second command's text never got typed because the first
   `xrscreen.py` poll had already interrupted the CPU once.)

   Caveat: even with `-ui null -ao null`, XRoar's CPU is *not* dramatically
   faster than a real CoCo's — a 2000-iteration empty `FOR/NEXT` loop takes a
   few real seconds, similar to real hardware. (The near-100% CPU usage you
   see in `ps` while it's running is XRoar's pacing loop spinning, not a sign
   the emulation is running drastically faster than real time.) Size
   `--timeout` accordingly for whatever the target program actually does.

   Caveat: `-ui null` does not react to `SIGTERM` in testing here (it kept
   running); use `kill -9 <pid>` (or `pkill -9 -f 'xroar -machine'`) to stop
   headless instances started this way.

On a PiCoCo: copy `PICOCO.DSK` to the flash (`fs export`), `dw mount 3
PICOCO.DSK`, `save`, then on the CoCo `DRIVE 3:RUN"PICOCO"`.
Needs firmware 1.2 or later and `becker native`.
