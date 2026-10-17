# coco/: the Loader, the Stub and the CoCo-side test tools

Build: `make` (needs lwasm from LWTOOLS and ToolShed `decb`). It builds the
Loader disk `PICOCO.DSK` and the Stub, and regenerates
`firmware/src/ui/manager_rom.h`.

## Toolchain

- **LWTOOLS** (`lwasm`, `lwlink`, `lwar`) and **XRoar** via `brew install lwtools xroar`.
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

### Banked images and `$FF40`

A 32K, 64K or 128K image loads as 2, 4 or 8 banks of 16K; a write to `$FF40`
selects the bank (bits 2-0). The switch takes effect one fetch late at both
CPU speeds: the instruction fetch right after `STA $FF40` still comes from
the old bank, every fetch after that from the new one (firmware
`docs/firmware-architecture.md` §3.2.4). So bank-switching code that runs
from the cart must keep the instruction after the `STA` identical in every
bank — put the switch routine's tail at the same address in each bank, the
usual practice for self-switching paks. Switching from RAM needs nothing.

## Emulate

```
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host
mkdir -p /tmp/picoco-pcdisk && cp coco/PICOCO.DSK /tmp/picoco-pcdisk/
build-host/picoco-host --dir /tmp/picoco-pcdisk --mount 0=PICOCO.DSK &
xroar -machine coco2bus -becker -rompath ~/.xroar/roms
```

At the `OK` prompt type `RUN"PICOCO"`. The Loader enters the Manager, or prints `PICOCO NOT RESPONDING` then `OK` when the firmware does not answer.

For the CoCo 3 profile, add `-cart-rom hdbdw3bc3` (see below for why):

```
xroar -machine coco3 -cart-rom hdbdw3bc3 -becker -rompath ~/.xroar/roms
```

Same result: `RUN"PICOCO"` enters the Manager or prints `PICOCO NOT RESPONDING`. (If a local
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
   PICOCO NOT RESPONDING
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


## Manager stub in XRoar

`picoco-host --ui` treats the whole TCP stream as a UI session (XRoar's Becker
cart forwards only `$FF41`/`$FF42`, so the stub's `$FF43` write never arrives).
Install the stub with `cp coco/stub/manager.rom ~/.xroar/roms/manager.rom`,
then (`D` = a scratch dir holding `CARTTEST.ROM` and `HDB.ROM`; add
`-ui null` to run headless). Launch each XRoar with `pkill -9 xroar` first: a
stale instance keeps the GDB port and `xrscreen.py` then hangs. Wait ~12 s
before `xrscreen.py` when `-type` has several keys.

1. No Extended BASIC (proves the first screen, the probe and the diff drawing;
   the highlighted row prints lowercase in `xrscreen.py`, it is inverse video):
   ```
   build-host/picoco-host --dir "$D" --port 65511 --ui &
   xroar -machine coco2bus -no-extbas -becker -becker-port 65511 -cart-rom manager \
         -no-cart-autorun -rompath ~/.xroar/roms -gdb -ao null -type 'EXEC 49154\r' &
   python3 coco/tools/xrscreen.py --wait-for 'PICOCO  32K NO ECB' --wait-for 'carttest.rom'
   ```
2. Keys and launch handshake (`-type 'EXEC 49154\r\n\r'` moves down to `HDB.ROM`
   and ENTER draws `NEEDS EXTENDED BASIC`; `-type 'EXEC 49154\r\r'` runs
   `rom launch CARTTEST.ROM` and the screen returns to the manager). Finding:
   XRoar's `-type` does deliver keys to the stub's `JSR [$A000]` (POLCAT).
3. Extended BASIC autostart (no typing; proves `DK` at `$C000` is found and
   `$C002` called, row 0 reads `32K ECB`):
   ```
   xroar -machine coco2bus -becker -becker-port 65511 -cart-rom manager \
         -rompath ~/.xroar/roms -gdb -ao null &
   python3 coco/tools/xrscreen.py --wait-for 'PICOCO  32K ECB'
   ```

4. CoCo 3 (autostart, no typing; `-machine coco3`, no `-no-cart-autorun`):
   ```
   xroar -machine coco3 -becker -becker-port 65511 -cart-rom manager \
         -rompath ~/.xroar/roms -gdb -ao null -ui null &
   python3 coco/tools/xrscreen.py --wait-for 'COCO 3'
   ```
   Row 0 reads `PICOCO  COCO 3 128K` (`-ram 128`; `512K`, `1M`, `2M` with `-ram 512`, `1024`, `2048`) and the two files are listed. XRoar
   cannot swap its cart ROM, so the `$16`/`$17`/`$18` leave paths are bench
   steps. `-type '\n\r'` (the `$17` restart) was tried but the stub's screen
   never changed (no BASIC prompt exists to take the keys), so that check is
   left to the bench.

   **Super ECB patches a `DK` cart.** On a CoCo 3, Super Extended BASIC copies
   the cart into RAM and patches the copy as if it were Disk BASIC (coco3.rom
   `$C321`): 3 bytes at `$C0D9` and 11 NOPs at `$C8B4`, or at `$C0C6` when the
   byte at `$C004` is `$D6`. The stub therefore keeps only a `jmp` at `$C002`,
   real code from `$C100`, and `coco/stub/Makefile` fails the build if those
   zones are not zero (before this, the patch landed mid-`POLL` and the first
   poll always failed with `PICOCO NOT RESPONDING`).

   Two harness traps: `picoco-host` exits when its stdin hits EOF, so run it
   as `(sleep 3000 | build-host/picoco-host --dir "$D" --port 65511 --ui &)`;
   and XRoar's GDB stub on `coco3` often never answers the first read (a
   timed-out client also uses up the single connection), so restart XRoar
   until `xrscreen.py` gets through.

## On the bench (real hardware)

1. Copy `PICOCO.DSK` to the PiCoCo's flash: `fs export`, drag it onto the
   `PICOCO` volume, `fs import` (see `firmware/README.md` "Export").
2. `dw mount 3 PICOCO.DSK`, then `save`.
3. Needs firmware 1.2 or later, and `becker native` (`becker native` then
   `save` if the board isn't already in that mode).
4. On the CoCo: `DRIVE 3:RUN"PICOCO"`. The `DRIVE 3:` prefix matters —
   the `PICOCO.BAS` loader `LOADM`s from BASIC's default drive.

## Using the Manager

The Manager is the firmware's screens, drawn on the CoCo by the Stub
(`firmware/src/ui/ui.c`). Ways in:

- Power on with the Manager as the saved ROM.
- CoCo 3: double RESET.
- CoCo 2: double RESET, then `EXEC 49154` (a CoCo 2 RESET is a warm start; #71 tracks starting it through /CART).
- From HDB-DOS with `PICOCO.DSK` mounted: `RUN"PICOCO"` (the Loader).

Console verbs are in `firmware/README.md`.

Home: drives D0-D3 and `ROM NEXT`, then the menu ROMS, DISKS, SETTINGS,
WIFI. Up/Down move, ENTER opens, BREAK returns to BASIC.

- **ROMS**: ENTER launches the selected ROM. `D` sets it as the next-boot
  ROM (`rom boot`); `*` marks the current next-boot ROM. BREAK goes back.
- **DISKS**: a digit mounts the selected image in that drive. `E` (the
  screen shows `EJECT: PRESS 0-3`) then a digit ejects that drive. `N`
  prompts `NAME:` and creates a new blank disk image. `B` boots the
  selected image (below). BREAK goes back.
- **BOOT**: `B` in DISKS mounts the selected image in drive 0 (replacing
  what was there; it needs native transport, else `BOOT NEEDS NATIVE MODE`).
  An OS-9 disk boots at once (`NEEDS 64K` on a smaller machine). Any other
  disk opens the Boot list of its BAS/BIN files. ENTER on a file:
  after a Loader entry, BASIC types `DRIVE0:RUN"X"` or
  `DRIVE0:LOADM"X":EXEC` itself; after a power-on entry the Manager shows
  `STARTING DOS, THEN TYPE:` and the line, and any key cold-starts the
  default DOS ROM (the one `D` marked in ROMS, not necessarily HDB-DOS).
  With no DOS ROM set it says `SET A DOS ROM AS DEFAULT`; on a Color BASIC
  machine, `NEEDS EXTENDED BASIC`. BREAK goes back to DISKS with drive 0
  still mounted.
- **SETTINGS**: `H` toggles HDB-DOS drive mode, `T` sets the clock, `Z` the
  timezone. BREAK goes back.
- **WIFI** (Plus-W only): `S` scans for an SSID, `P` sets the PSK, `H` sets
  the server host, `M` toggles the next-boot transport (net or native) and
  shows `(RESET TO APPLY)`. BREAK goes back.

In every list, SHIFT+UP/DOWN pages, and `G` then up to 4 letters jumps to
the first name with that prefix. Every change saves at once; there is no
save key. In the line editor (SSID, PSK, host), SHIFT+0 toggles lowercase.
