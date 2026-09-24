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

## On the bench (real hardware)

1. Copy `PICOCO.DSK` to the PiCoCo's flash: `fs export`, drag it onto the
   `PICOCO` volume, `fs import` (see `firmware/README.md` "Export").
2. `dw mount 3 PICOCO.DSK`, then `save`.
3. Needs firmware 1.2 or later, and `becker native` (`becker native` then
   `save` if the board isn't already in that mode).
4. On the CoCo: `DRIVE 3:RUN"PICOCO"`. The `DRIVE 3:` prefix matters —
   the `PICOCO.BAS` loader `LOADM`s from BASIC's default drive.

## Using the manager

On startup the program runs `version` to check the link; if it times out,
it shows `PICOCO NOT RESPONDING` and `(BECKER NATIVE? FIRMWARE >= 1.2?)`,
then `R=RETRY  BREAK=EXIT` — press `R` to try again (e.g. after switching
the board to `becker native`) or `BREAK` to give up and return to BASIC.

Main screen keys:
- `0`-`3`: mount the selected image in that drive.
- `SHIFT+E` then `0`-`3`: eject that drive.
- `SHIFT+N`: prompt for a name (`.DSK` appended if there's no extension),
  `fs new` a blank image. Refused (with the firmware's error text) if the
  name already exists, is 32 characters or longer, is `picoco.cfg`, or
  holds a `/`, `\`, `:` or control byte.
- `SHIFT+B`: boot the selected image (see Boot below).
- `SHIFT+S`: settings screen.
- `SHIFT+V`: save (`save`) — persists mounts, next-boot ROM choice, and
  HDB-DOS drive mode.
- Up/Down arrows: move the selection; `SHIFT`+arrows: page up/down.
- Letters: jump to the first file name starting with that letter (type
  several quickly to narrow further, SDC Explorer style).
- `BREAK`: exit to BASIC (warm start, no hardware reset). If anything
  changed since the last save, offers save-then-exit or stay.

The file list excludes `*.ROM` and `picoco.cfg`. If the firmware's reply
was too big to fit (its `...` truncation marker) or the flash holds more
than 128 matching files, the program shows `LIST TRUNCATED` once after
loading — not every file may be listed.

A CoCo reset after `SHIFT+V` (save) keeps the mounts; a reset without
saving drops them, since every `/RESET` reboots the Pico, which replays
`picoco.cfg` from scratch. `save` writes a `dw disk insert <n> <file>`
line for each read-write mount (so a name with spaces, e.g. "my
disk.dsk", survives the round trip — `dw mount` only takes one token) and
a `dw mount <n> <file> ro` line for each read-only mount.

### Settings screen (SHIFT+S)

- `R`: pick a `.ROM` file for the *next* boot (`rom boot`) — does not
  swap the ROM currently driving `/CTS`. Shows `SAVE, THEN RESET`
  afterward. Hides any `.ROM` file whose name contains a space, since
  `rom boot` takes a single token and couldn't be sent one.
- `H`: toggle HDB-DOS drive-by-LSN addressing (`dw hdbdos on|off`).
- `T`: set the clock (`YYYY-MM-DD HH:MM`), converted to Unix seconds and
  sent as `time set`. Shows `(LOST AT RESET)` under the current time if
  the firmware reports `clock lost` (see
  `docs/superpowers/specs/2026-09-23-coco-manager-design.md` §4.5; the
  AON bench check is still pending).
- `V`: save. `BREAK`: back to the main screen.

### Boot (SHIFT+B)

1. Mounts the selected image to drive 0.
2. Reads track 34 sector 1 (LSN 612).
3. If it starts with `OS`: loads all 18 sectors of track 34 to
   $2600-$37FF and jumps to $2602, as Disk BASIC's `DOS` command does
   (OS-9 boot; not exercised in XRoar — no Becker OS-9 boot disk
   available there — so this path is bench-only).
4. Otherwise: reads the RS-DOS directory (track 17, sectors 3-11), lists
   the `.BAS`/`.BIN` entries, and hands the picked one to BASIC as
   `DRIVE0:RUN"NAME"` or `DRIVE0:LOADM"NAME":EXEC`. Always `DRIVE0:`:
   the booted image is in drive 0, but the manager itself may have been
   launched from another drive (step 4 of "On the bench" above uses
   drive 3), which would otherwise be BASIC's default drive for the
   handoff command.

## Memory layout

- The 2-line `PICOCO.BAS` loader sits at $2601: line 10
  (`IF PEEK(65534)=140 THEN WIDTH32`) sets 32-column mode on a CoCo 3,
  line 20 (`LOADM"PICOCO":EXEC`) loads and runs the program.
- The `fs ls` reply buffer (`LSBUF`, 4120 bytes: a 23-byte `OK` header
  plus a 4096-byte body plus a NUL) lives at a fixed $2700-$3717 instead
  of BSS, between the loader and the program, freeing that space for the
  program itself. `ui_run()` refuses to start (`BASIC PROGRAM TOO BIG`)
  if BASIC's array-end pointer (`ARYEND`, $001F) already reaches past
  $2700 — i.e. the BASIC program that `RUN`s `PICOCO` is too large.
- `PICOCO.BIN` loads at $3800 (`ORG` in `coco/Makefile`).
- OS-9 boot (track 34 -> $2600-$37FF) and the `RUN"X"`/`LOADM"X"`
  handoff both overwrite $2700-$37FF, but only after the picked file's
  name has already been copied out of `LSBUF`, so the overlap is safe.

## Developer notes

- **CMOC 0.1.100 miscompiles `!func(...)` used as a value** (verified
  on-device): `if (!f())` is fine, but assigning the negation of a
  direct function-call result (e.g. `keep = !ends_with_ci(...)`) is not.
  Write `f() == 0` instead. See the `ponytail:` comment in
  `coco/parse.c`'s `parse_ls`.
- **`cls` clashes with CMOC's `coco.h`**, which already declares one;
  this program's screen-clear function is named `clear_screen` instead.
- Keep using the headless XRoar recipe above for anything that doesn't
  need real hardware. `picoco-host` doesn't exercise the OS-9 boot
  branch or go past what the RVEC4 handoff spike already covers — see
  `docs/superpowers/specs/2026-09-23-coco-manager-design.md` §5.5, which
  passed in XRoar on both the CoCo 2 and CoCo 3 profiles.
