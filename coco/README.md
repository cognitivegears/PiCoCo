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
xroar -machine coco2bus -cart becker -becker -rompath ~/.xroar/roms
```

At the `OK` prompt type `RUN"PICOCO"`. Expect `PICOCO HELLO` then `OK`.
Repeat with `-machine coco3` for the CoCo 3 profile — same result. (Local TCP
listen/connect for `picoco-host` and XRoar's becker port may need
`sandbox.network.allowLocalBinding: true`, or running outside the sandbox.)

Notes on the exact flags, confirmed against this XRoar build (1.12.1):

- There is no cart profile literally named `hdbdos`. The predefined cart
  profile that does what we want is `becker` (`RS-DOS with becker port`,
  `-cart-type rsdos`), which loads ROM list `rsdos_becker` = `hdbdw3bck` (or
  `coco3`'s own default set for `-machine coco3`, which needs no `-cart` at
  all — `-cart becker` still works there too since HDB-DOS auto-detects the
  becker port). So the command is `-cart becker -becker`, not `-cart hdbdos`.
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
   the already-running emulator" CLI command. To send a different command to
   an already-booted instance, restart XRoar with a new `-type` string (cheap:
   boot to `RUN"PICOCO"` takes well under a second under `-no-ratelimit`-like
   full-speed null audio).

2. **Headless video/audio + screen capture**: `-ui null -ao null` runs XRoar
   with no window and no audio backend (confirmed working on this build,
   1.12.1 macOS/SDL2 build) — no dummy display needed. Combined with `-gdb`
   (default port 65520), `coco/tools/xrscreen.py` (stdlib-only Python 3)
   connects to the GDB remote stub, reads $0400-$05FF (the 32x16 text screen)
   with a `$m400,200#xx` packet, and prints the decoded screen as 16 lines of
   32 characters:
   ```
   python3 coco/tools/xrscreen.py [host] [port]   # defaults 127.0.0.1 65520
   ```
   VDG decode: for each screen byte `b`, `c = b & 0x3F`; the character is
   `chr(c + 0x40)` if `c < 0x20` else `chr(c)`. Bit 6 clear means inverse
   video, marked here by lowercasing the character. Bit 7 set means
   semigraphics, printed as `#`. The blinking text cursor renders as a
   semigraphics block, so a trailing `#` on the last non-blank line is normal
   and just the cursor.

   Full worked example (CoCo 2, after `picoco-host` is up):
   ```
   xroar -machine coco2bus -cart becker -becker -rompath ~/.xroar/roms \
         -ui null -ao null -gdb -gdb-port 65520 -type 'RUN"PICOCO"\r' &
   sleep 2
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

   Caveat: with `-ui null -ao null` XRoar runs the CPU at full host speed
   (nothing throttles it to real time), which is fine for scripted boot/run
   checks but means don't rely on wall-clock timing assumptions.

   Caveat: `-ui null` does not react to `SIGTERM` in testing here (it kept
   running); use `kill -9 <pid>` (or `pkill -9 -f 'xroar -machine'`) to stop
   headless instances started this way.

On a PiCoCo: copy `PICOCO.DSK` to the flash (`fs export`), `dw mount 3
PICOCO.DSK`, `save`, then on the CoCo `DRIVE 3:RUN"PICOCO"`.
Needs firmware 1.2 or later and `becker native`.
