# PiCoCo firmware bench test plan

Checklist for the manual tests that need hands on the hardware. Run it
with Claude: you do the wiring and meter readings, Claude drives the
console over USB and records results. Each step says what to do, what
Claude will run, and what "pass" looks like.

The console port is the second `/dev/cu.usbmodem*`. The bench board is
PCB v2.3.1 #1 (Pico 2); the Plus-W PCB has not run in a CoCo.

## Already verified on this Pico 2 (no CoCo)

| Check | Result |
|---|---|
| USB enumerates: two serial ports + storage interface | pass (after allowing the accessory in macOS Privacy & Security) |
| `version`, `status`, `log dump`, `help` | pass |
| `smoke` (GP0..GP7 + LED), `halt on` / `halt off` accepted | pass (GP27's electrical check: PCB phase 4, pcb-bringup) |
| `fs format`, `fs ls`, `save`, reboot, config replay | pass |
| `dw selftest` (create, mount, READ, WRITE, READEX, eject) | pass |
| `fs export` mounts as `PICOCO` on the Mac, copy a `.dsk`, eject, `fs import`, file persists across reboot | pass |
| Guards: export/format refused with a drive mounted or capture open; fs/dw commands refused while exporting; import refused before host eject | pass |
| `crash` and `crash panic` leave a record that `status` shows after reboot | pass |
| `reboot` reports `last reset reboot`, hard fault reports `hardfault pc=...` | pass |
| core1 running: `log dump` shows `core1 up, halt released` | pass |
| Address decode, data byte on the pins, /HALT drive | `bus selftest` (K.1) and the PCB rows (F) cover them |

## F. PCB bring-up (v2.3.1): two boards, real CoCo hardware

One board built with a Pico 2, one with a Waveshare RP2350B-Plus-W
(`-DPICOCO_BOARD=plusw`). Test both on CoCo 1, CoCo 2, CoCo 3, bare and
through a Multi-Pak Interface (MPI).

### F.1 Pre-power checks (both boards)

- JP2 at 1-2 (hardware /OE), JP3 at 1-2 (audio on header pin 34), JP4 open
  (no autostart tie), JP5 open (no firmware /CART or /NMI drive yet).
- R7 = 10 kΩ (the v2.3.1 value; 100 kΩ loses to the RP2350's own reset
  pull-down and never releases /HALT).
- Plus-W builds only: the radio's GP36-GP39 are verified (2026-09-29,
  `firmware/README.md` "WiFi"). Firmware blinks LED2 on GP23; after
  flashing, check the LED blinks. Also confirm 16 MB flash on the module (no
  runtime JEDEC ID check exists; `PICO_FLASH_SIZE_BYTES` in
  `firmware/boards/picoco_plusw.h` is a build-time assumption, not verified
  against the part actually on the board).

### F.2 Per-build bring-up steps

1. Build and flash:
   - Pico 2: `cmake -B build-pico -G Ninja -DPICO_SDK_PATH=... firmware &&
     ninja -C build-pico`, then `flash.sh build-pico/picoco.uf2`.
   - Plus-W: same with `-DPICOCO_BOARD=plusw` into `build-pico-plusw`.
2. Power up with the cart seated; /HALT should hold (CoCo dead) until
   `log dump` shows `core1 up, halt released`.
3. `status`: check cycle counts are moving and `bus engine_stall 0`,
   `event_drop 0`, `event_lap 0`.
4. Becker loop, no ROM needed: `becker loop`, `bus drive on`, then on the
   CoCo `POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` →
   `0 2 65`.
5. `rom load <rom>` (see F.3 for which file), `save`, power-cycle: HDB-DOS
   boots, `DIR` lists the test disk, `LOADM"DINORUN":EXEC` runs it.
6. `SAVE` a program from BASIC, power-cycle, `DIR` still lists it.
7. `dw stats`: `crc_err 0` throughout.

### F.3 ROM notes

- CoCo 1 and CoCo 2: `hdbdw3bck.rom` (`rom load hdbdw3bck.rom`, then `save`).
  `hdbdw3bc3.rom` (bc3, 1.79 MHz during transfers) is CoCo 3 only.
- CoCo 1 needs Extended Color BASIC (not plain Color BASIC) for HDB-DOS to
  hook in.
- On CoCo 1/2, BASIC `PEEK(&HC000)` reaches the cart directly (unlike a
  CoCo 3, which runs BASIC from RAM) — they execute the DOS ROM as code from
  the cart, so an address-decode error crashes immediately instead of
  showing up as a stray PEEK. Watch `engine_stall` and `event_drop` closely
  on these two.
- Confirm the test disk image is readable from CoCo 1/2 DOS before blaming
  the hardware for a failed `DIR`.

### F.4 Through an MPI

- Set both the /CTS and /SCS slot-select switches to PiCoCo's slot.
- An unmodified Tandy 26-3024 MPI needs the CoCo 3 upgrade to work with a
  CoCo 3; a 26-3124 works as shipped.
- The MPI's own buffers add delay, so the CoCo 3 fast-mode (bc3) row through
  the MPI is the read path's timing-margin test.
- A working MPI row is also the first data point for the unverified /SLENB
  item in `docs/hardware-design.md` §9 (deferred to v2.4).
- Also try a second cart in another MPI slot, and switch PiCoCo between
  slots, to rule out a slot-specific fault.

### F.5 Test matrix

Rows: CoCo model / mode. Columns: board. Cells: pass/fail + date, or blank
until run.

| CoCo | Pico 2, bare | Pico 2, MPI | Plus-W, bare | Plus-W, MPI |
|---|---|---|---|---|
| CoCo 1 (`hdbdw3bck`) | | | | |
| CoCo 2 (`hdbdw3bck`) | electrical pass 2026-10-01 (section I); cart test ROM 55 min clean after the read-path fix (I.5); HDB-DOS not run, the 26-3026 has no ECB | | | |
| CoCo 3, 0.89 MHz (`hdbdw3bck`) | | | | |
| CoCo 3, 1.79 MHz (`hdbdw3bc3`) | | | | |

## G. WiFi (Plus-W)

Firmware 1.3, Plus-W build. Server: `picoco-host` on the Mac unless noted.

### G.1 Bare module over USB (no CoCo)

Run 2026-09-30, results in G.3 (scan, join, errors, fallback, SNTP).
`bus selftest net` is not supported by the PIO engine: it answers `err
bus selftest net: not supported by this engine`.

### G.2 On the PCB with a CoCo

- Boot hold with a real CoCo: HDB-DOS boots over WiFi with /HALT released at link-up.
- DIR and LOADM from DW4 over WiFi.
- DIR and LOADM from FujiNet-PC over WiFi (BoIP).
- Fallback with the server stopped: CoCo boots native after 10 s.
- Manager WiFi screen end to end (scan, PSK, server, mode, save).
- VSYS scope trace during a LOADM over WiFi, against the 300 mA cart budget.

### G.3 Results

| Check | Result | Date |
|---|---|---|
| G.1 join + DHCP + connect at boot | 5.3 s (`net up in 5327 ms`) | 2026-09-30 |
| G.1 OP_TIME round trip | 8-13 ms typical, worst 23-30 ms over 20 | 2026-09-30 |
| G.1 `bus selftest net` | pass (CPU loop, firmware 1.3). Not supported by the PIO engine (G.2 on a CoCo covers the transport) | 2026-09-30 |
| G.1 bad SSID / closed port / bad DNS name | `no such network` / `refused` / `dns failed`, ~10 retries in 22 s each | 2026-09-30 |
| G.1 server down at boot | `net failed (refused), native fallback` after 10 s; `becker net` re-arms | 2026-09-30 |
| G.1 `net scan` | 21 networks | 2026-09-30 |
| G.1 cold-boot SNTP seeding | pass 2026-09-30: after a USB power cycle the boot log shows `net: sntp seeded 1790778946`, `time` reports it with `clock kept`, `net up in 8329 ms` | |
| G.1 `net tz` (firmware 1.3, 25517de) | pass 2026-09-30: `net tz -240` + `save` + reboot logs `net: sntp set 1790765675 (utc 1790780075 tz -240)`, exactly Mac UTC minus 14400; `net tz 0` then `net tz -240` move `time` at once, no hourly wait | |
| G.1 reconnect after a Pico reboot with picoco-host (af12f4d) | pass 2026-09-30: the server replaces the stale client on the new accept; `bus selftest net` passes right after the reboot (dwinit 5 ms, worst 19 ms) without restarting the server. `bus selftest net` is not supported by the PIO engine | |
| G.2 boot hold with CoCo | | |
| G.2 DIR/LOADM from DW4 | | |
| G.2 DIR/LOADM from FujiNet-PC | | |
| G.2 fallback, server stopped | | |
| G.2 manager WiFi screen | | |
| G.2 VSYS scope trace | | |

## H. NitrOS-9 over Becker

CoCo 3 only (Level 2). Two disks, easiest first: a stock NitrOS-9 Becker
boot floppy served from flash, then Ease of Use (EOU) from a server.
NitrOS-9 L2 runs the CoCo 3 at 1.79 MHz, so every row here is also a
fast-mode Becker test whichever HDB-DOS ROM started it.

### H.0 What is needed

- **CPU match.** The EOU zip in the repo root is the 6309-only build. On a
  stock 68B09E CoCo 3 it will not boot; that needs the 6809
  build (`68SDC.VHD`) instead. Same rule for the stock disk (`6809l2` vs
  `6309l2`).
- **512 K RAM** for EOU. The stock disk runs in 128 K.
- **Stock disk:** a `*coco3_becker.dsk` from a NitrOS-9 release (here
  `nos96309l2v030300coco3_becker.dsk`; the `_headless` variant puts the
  console on a DriveWire virtual terminal and is not wanted). Its kernel track carries
  `boot_dw_becker` and `/DD` is `/X0`, so it boots with nothing but
  DriveWire. Small enough for the flash filesystem on either board.
- **EOU:** `63SDC.VHD` (or `68SDC.VHD`) does **not** boot on PiCoCo as
  shipped: its kernel track is `boot_sdc` and every boot set has the CoCo
  SDC as `/DD`; `OS9Boot.dw` uses the bitbanger `dwio`. `63EMU.DSK` is the
  emulator hard-disk boot and has no DriveWire at all. Everything for a
  Becker boot is on the VHD, so H.1 remasters it.
- **EOU needs a server.** The VHD is 128 MB; the flash filesystem is 2.5 MB
  (Pico 2) or 14.5 MB (Plus-W). Serve it from `picoco-host` over
  `becker bridge` + `tools/becker_relay.py` (either board) or `becker net`
  (Plus-W). Round trip must stay under 200 ms.

### H.1 Prep: EOU Becker image

`python3 firmware/tools/eou_becker.py 63SDC.VHD 63BECKER.VHD` (needs
toolshed `os9`). It copies the image, then:

- Kernel track (LSN 612): `KERNEL_TRACKS/kernel.dw` with `Boot` swapped for
  `boot_dw_becker`.
- Boot file: `BOOTS/OS9Boot.dw` with `dwio` -> `dwio_becker.sb`, `DD` ->
  `ddx0.dd`, and `RBSuper`, `llcocosdc`, `H1` removed (the SDC registers
  overlap the Becker port at `$FF41/$FF42`). Written over `OS9Boot.dw`'s
  own extent (`os9 gen` fragments a new file there) and LSN 0
  `DD.BT`/`DD.BSZ` pointed at it.

So SWAPBOOT's "dw" set is now the Becker one; picking any other set
un-does the boot pointer (re-run the script).

**HDB-DOS translation must be off before `DOS`** on any hard-disk image:
`picoco-host --hdbdos off`, or `dw hdbdos off` for a native mount. The
NitrOS-9 Boot module sends no DWINIT, and with translation on its boot-file
reads (LSN 47253 here) resolve to an unmounted drive 75: the screen shows
`KREL Boot Krn tb0....bt*j` then `NITROS9 6309 FAILED`, and the server
counts `notrdy`. The stock floppy is unaffected (everything below LSN 630).

### H.2 Emulator gate (no hardware)

1. `./build-host/picoco-host --dir <dir> --mount 0=<image> --hdbdos off`
   (pick another `--port` if 65504 is taken, and match `-becker-port`).
2. `xroar -machine coco3 -machine-cpu 6309 -ram 512 -cart becker
   -becker-ip 127.0.0.1 -becker-port 65504 -cart-rom firmware/roms/hdbdw3bc3.rom
   -type 'DOS\r'` (drop `-machine-cpu` for a 6809 image).
3. Pass: stock disk reaches the shell prompt; EOU reaches its banner with
   `DriveWire - (Installed) (Active)`. `Ctrl-C` on `picoco-host`: no CRC
   errors, `notrdy=0`.

`picoco-host` answers OP_TIME with Mac local time (since 2026-10-01; the
XRoar runs above predate that and stopped at EOU's `Time ?` prompt).

### H.3 Stock disk on the CoCo (native, from flash)

`fs export`, copy the `.dsk`, `fs import`, `dw mount 0 <file>`, then:

1. `DOS` from HDB-DOS: "NITROS9 BOOT", module list, shell prompt.
   `status`: `crc_err 0`, `becker underrun 0`, `dw hdbdos off` (DWINIT
   cleared it).
2. `date -t` matches the Pico's `time` (Clock2 over OP_TIME).
3. `dir -e /dd`; `dir /x1` with a second image on drive 1.
4. Write: `copy /dd/startup /dd/t1`, `list /dd/t1`, `del /dd/t1`. `status`
   writes count moves, no error; file survives a reboot if left in place.
5. `dw disk show` inside NitrOS-9 lists the mounts (virtual channel,
   `dw_vser.c`); `dw disk insert 1 <file>` then `dir /x1`.
6. Tick loss (`docs/ADDITIONAL_ROADMAP.md` §5 item 2): note `date -t`, run
   `dir -e -r /dd >/nil` in a loop for 10 minutes, compare with the Pico's
   `time`. Record the drift in seconds.
7. CoCo RESET returns to HDB-DOS and `DOS` boots again; same after a
   power cycle with `dw mount` saved in `picoco.cfg`.
8. Manager: `B` on the OS-9 disk boots it (never bench-tested; the plan
   skipped it for lack of a disk).

### H.4 EOU on the CoCo (served)

Transport per H.0; `picoco-host --mount 0=63BECKER.VHD --hdbdos off`.

1. `DOS`: boots to the EOU prompt; `startup` loads all fonts with no
   retry stall. Record the boot time.
2. `date -t` matches the server (with `picoco-host`: answer `Time ?` by
   hand, see H.2); `free /dd` and `dir -e /dd/cmds` work.
3. `gshell`: desktop draws, mouse or keyboard moves, launch one app and
   quit back.
4. Write: copy a >100 K file, `dcheck /dd` reports no errors.
5. 30-minute soak in gshell with a second window running `dir -e -r /dd`:
   `status` still `crc_err 0`, `underrun 0`; relay or `net status` shows
   no reconnect.
6. `swapboot` runs and lists the sets; quit without selecting (only the
   current boot works here, see H.1).
7. Plus-W: repeat 1, 2 and 5 over `becker net`.

Not in scope until the sound/MIDI work lands: `/MIDI`, `/P`, `/N` channels.

### H.5 Results

Firmware fixes that came out of this section (2026-10-01), details in
`docs/pcb-bringup.md` "NitrOS-9 bring-up":
- core1 Pico 2 read path: latch preloaded during idle, one store on OE_BUS
  low; pad discharge after every cycle. Sector checksum failures over the
  bridge went from 80 in 1,966 to 1 in 1,888 on an EOU boot.
- DriveWire server drops the CoCo SDC probe (`64 64 00 64 00 ...` on `$FF42`).
- Bridge mode holds the CoCo's bytes until CDC0 has a listener (HDB-DOS
  reads drive 0 at power-on with no timeout).
- `bus selftest` on a bare Pico 2 (chip rev A2), six runs in one boot, all
  pass: read answered ~139-146 ns after OE_BUS falls from idle (the
  2026-09-30 loop measured 264-286 ns with the same sweep, budget 230),
  256-286 ns back to back, 366-396 ns straight after a write.
- RP2350-E9 confirmed on that Pico: D0-D7 driven high then released onto the
  internal pull-downs still read `ff` after 51 ms; driven low then released
  read `00` (`selftest pad_hold` line).
- `bus selftest` on a bare Plus-W (chip rev A4), three fresh boots, all
  pass with the ported loop (latch preload at Q time, pad discharge):
  response 132 ns after E rises, burst 381 ns (old loop 139-154 / 352-366).
  `pad_hold` reads `00`: no E9 on A4.
- Open: Plus-W on a PCB with a CoCo (section G.2 and F.5), including a
  scope look at JP2 2-3; a second `bus selftest` in one boot fails
  `read_bank0_marker` on the Plus-W (old loop too).
  Closed 2026-10-02: the JP2 2-3 scope look is moot (the PIO engine needs
  JP2 at 1-2), and `read_bank0_marker` went with the old self-test.


Bench CoCo 3 CPU: 6309  RAM: 2 MB

| Check | Pico 2 | Plus-W | Date |
|---|---|---|---|
| H.1 EOU Becker image built | n/a | n/a | 2026-10-01: `63BECKER.VHD`, 64 modules, boot file 35361 bytes at LSN 47253 |
| H.2 XRoar, stock disk | n/a | n/a | pass 2026-10-01: shell prompt (6309, hdbdw3bc3) |
| H.2 XRoar, EOU | n/a | n/a | pass 2026-10-01 to the `Time ?` prompt: 512k, 6309 native, DriveWire active; `reads=1383 crc_err=0 notrdy=0`. gshell not run |
| H.3.1 stock boot | pass 2026-10-01 after the E9 discharge fix in `bus_core1.c`: shell prompt, `dw reads 308 crc_err 0`, `becker reads 79457 underrun 0`, `dw hdbdos off`. Before the fix: hung at `i2x`, underrun 1, crc_err 84 (phantom byte after a `$26` write) | | 2026-10-01 |
| H.3.2 clock | pass: startup prints the 2014 build date (before the first OP_TIME is applied), `date -t` then shows 2026-10-01 10:38:51 matching the Pico | | 2026-10-01 |
| H.3.3 read, two drives | `dir -e /dd` pass; `/x1` not run | | 2026-10-01 |
| H.3.4 write | pass: copy, list, del; `dw reads 364 writes 16 crc_err 0`, `underrun 0` | | 2026-10-01 |
| H.3.5 `dw` utility | | | |
| H.3.6 tick loss in 10 min | | | |
| H.3.7 reset / power cycle | RESET inside NitrOS-9 restarts the NitrOS-9 boot, not HDB-DOS; power cycle returns to HDB-DOS (mounts need `save`) | | 2026-10-01 |
| H.3.8 manager `B` | | | |
| H.3 through an MPI | | | |
| H.4.1 EOU boot (bridge) | pass 2026-10-01 (third try): 2048k, 6309 native, boots to the prompt; 2409 sectors, 21 re-read after a checksum failure (0.9 %, open). Needed: the server dropping EOU's SDC probe (`64 64 00 64 00 ...` on `$FF42`), `COCO3FPGA=1` in the image. Still prompts for the time: root `startup` is the `.sdc` one | | 2026-10-01 |
| H.4.2-4 clock, gshell, write | gshell loads and launches apps (trackpad), no time prompt with the `.dw` startup, saved `becker bridge` survives a power cycle; ~7935 sectors, 42 re-reads (0.5 %, open), 0 phantom reads. `dcheck` not run | | 2026-10-01 |
| H.4.5 30 min soak | pass: copy + `dcheck /dd` + del over the bridge, no reconnect, no phantom reads. dcheck reports 117 lost and 129 shared clusters: both are in the untouched `63SDC.VHD` as shipped (kernel track, old boot extents; 5 files with two directory entries), so no damage from this session | | 2026-10-01 |
| H.4.6 swapboot | | | |
| EOU boot after the read-path rework | 1 re-read in 1,888 sectors (was 80 in 1,966), `oe_glitch 0`, `addr_resample` 70,314 of 1.3 M reads, `underrun 52` (all the SDC probe) | | 2026-10-01 |
| Re-sample before recompute (final build, 370603b) | EOU boot: 0 re-reads in ~1,880 sectors, `addr_resample 0` in 1.3 M reads, `oe_glitch 0`, `underrun 52` (the SDC probe) | | 2026-10-01 |
| H.4.7 EOU over WiFi | n/a | | |

## I. CoCo 2 (PCB v2.3.1 #1, Pico 2)

First run on a machine that executes the DOS ROM straight from the cart:
a CoCo 3 copies `$C000-$FEFF` to RAM once at boot, a CoCo 2 fetches every
HDB-DOS opcode through U10 at 0.89 MHz. One wrong ROM read crashes instead
of showing up as a retry. Bare slot only; the MPI rows stay in F.4.

### I.0 Before the board goes in (USB only, no CoCo)

The config saved after section H is wrong for a CoCo 2. Fix it first:

1. `status`: note `rom next` and the `becker` mode.
2. `becker native`. A saved `becker bridge` holds the CoCo at power-on
   until CDC0 has a listener.
3. `rom load hdbdw3bck.rom`. `hdbdw3bc3` pokes `$FFD9`, which on a CoCo 2 is
   the SAM rate bit: 1.79 MHz on a machine that cannot run it.
4. `dw mount 0 DINORUN.DSK`, `dw hdbdos on`, `bus drive on`, `save`,
   `reboot`, `status` again: `rom now hdbdw3bck.rom`, `becker` native.
5. Record the CoCo 2 in I.4: model number (26-3026/3027, 26-3127, 26-3134
   ...), RAM (16K/64K), BASIC versions from the power-on banner without the
   cart. Extended BASIC is required; 16K is enough for HDB-DOS but not for
   the manager or DINORUN.
6. Optional emulator pre-check, same ROM and disk:
   `xroar -machine coco2bus -becker -rompath ~/.xroar/roms` against
   `picoco-host` (`coco/README.md`).

### I.1 Boot and ROM path

CoCo off, board in, USB to the Mac, CoCo on.

1. Banner `HDB-DOS 1.5 BECKER COCO 2` over `EXTENDED COLOR BASIC`.
   `log dump` has `core1 up, halt released`. No banner: `status` first
   (cycles moving? `engine_stall`, `event_drop`), then TP1/TP5 on the scope.
2. `PRINT PEEK(&HC000);PEEK(&HC001)` → `68 75` ("DK"). On a CoCo 2 this
   reads the cart itself.
3. ROM checksum from BASIC, twice, same number both times:
   `S=0:FOR A=&HC000 TO &HDFFF:S=S+PEEK(A):NEXT:PRINT S`
   Expected `903857` (byte sum of `firmware/roms/hdbdw3bck.rom`). Takes
   about a minute.
4. `status`: `engine_stall 0`, `event_drop 0`, `event_lap 0`. (The
   2026-10-01 row below used the CPU loop's counters.)

### I.2 Becker and DriveWire

1. `POKE &HFF42,65:PRINT PEEK(&HFF41);PEEK(&HFF41);PEEK(&HFF42)` under
   `becker loop` → `0 2 65`; back to `becker native` after.
2. `DIR`, `LOADM"DINORUN":EXEC`, a `SAVE` + power cycle + `DIR`.
3. `DRIVE 3:RUN"PICOCO"`: manager draws in uppercase (no inverse-video
   garbage), E/N/V/S/B keys work, BREAK exits. First real-hardware run of
   the CoCo 2 path; only XRoar so far.
4. `status` / `dw stats`: `crc_err 0`, `underrun 0`, `event_drop 0`.
5. Soak: `10 DIR:GOTO 10` for 10 minutes, counters unchanged.

### I.3 Reset and power

1. RESET button: Pico reboots, HDB-DOS banner returns.
2. Cold power cycle with USB unplugged, five times: banner every time
   (the /HALT hold against a different power-on reset circuit).
3. `+5V` at the cart (TP or pad 39) with the board running: record it. The
   CoCo 2 supply is the weakest of the three.

Not in scope today: MPI, Plus-W board, NitrOS-9 Level 1 (needs 64K and a
`nos96809l1...coco1_becker.dsk`), 1.79 MHz (CoCo 3 only).

### I.4 Results

CoCo 2 model: 26-3026  RAM: 16K (`PRINT MEM` 14631)  BASIC: Color BASIC 1.2  ECB: none

No Extended BASIC, so HDB-DOS cannot start on this machine (only ECB looks
for `DK` at `$C000`): I.1.1 is the plain Color BASIC banner, and I.2.2-I.2.5
wait for an ECB ROM or another CoCo 2. Color BASIC has no `&H`; the decimal
forms are `PEEK(49152)`, `FOR A=49152 TO 57343`, `POKE 65346,65`,
`PEEK(65345)`, `PEEK(65346)`.

| Check | Result | Date |
|---|---|---|
| I.0 config: bck ROM, native, DINORUN on drive 0 | done: firmware 1.3; saved config had `hdbdw3bc3.rom` (mode already native), now `rom now load hdbdw3bck.rom`, drive 0 DINORUN.DSK, drive 3 PICOCO.DSK, `bus drive on`, `dw hdbdos on` after reboot | 2026-10-01 |
| I.1.1 banner, halt released | pass: `COLOR BASIC 1.2` banner and `OK` with the cart in (no HDB-DOS, see above); `core1 up, halt released` at 2.7 s | 2026-10-01 |
| I.1.2 `PEEK(&HC000)` = 68 75 | pass: `68 75` four times. One earlier `68 196` not reproduced (49193 holds 196; taken as a typo) | 2026-10-01 |
| I.1.3 ROM checksum x2 | pass: `903857` twice | 2026-10-01 |
| I.1.4 `addr_resample` / `oe_glitch` / `late_precompute` | 0 / 0 / 0; `bus reads 8205 writes 2`, exactly the PEEKs and POKEs typed (Color BASIC never touches the cart on its own) | 2026-10-01 |
| I.2.1 Becker loop `0 2 65` | pass | 2026-10-01 |
| I.2.2 DIR / LOADM / SAVE | | |
| I.2.3 manager | | |
| I.2.5 10 min DIR soak | | |
| I.3.1 RESET | pass: CoCo back to `OK`, Pico rebooted (uptime restarted, counters zeroed, `core1 up, halt released` at 2.7 s) | 2026-10-01 |
| I.3.2 cold boot x5, no USB | pass: banner and `OK` five of five; `PEEK(49152)`/`(49153)` = `68 75` on cart power alone | 2026-10-01 |
| I.3.3 +5V at the cart | pass: module pad 39 (VSYS, after D2) 4.8 V, TP6 3.27 V, measured against TP8. No +5V test point; finger 9 is inside the slot | 2026-10-01 |

### I.5 Without Extended BASIC: the cart test ROM

`coco/carttest.asm` (`make -C coco carttest.rom`, 8 KB) stands in for
HDB-DOS on a machine with no ECB. It runs from the cart, so every opcode is
a ROM read, and reads drive 0 LSN 0-629 with DriveWire OP_READ in a loop.

1. Copy `carttest.rom` to the flash volume (`fs export` / `fs import`),
   `rom load carttest.rom`. Never swap the ROM under a running test: power
   the CoCo off first (RESET is a warm start and keeps scribbled RAM).
2. Sum to expect, from the copy on the board (it changes with every SAVE):
   `python3 -c "print('%04X' % (sum(open('/Volumes/PICOCO/DINORUN.DSK','rb').read()) & 0xFFFF))"`
   while the volume is exported.
3. `EXEC 49152`. One line per pass (~14 s): `PASS nnnn SUM ssss ERR eeee`.
   A bad sector prints `Ecc llll` and the test carries on after a 0.5 s
   quiet wait. Any key stops it within one sector (a warm restart, back to BASIC).
4. `status` after: `underrun 0`, `engine_stall 0`, `event_drop 0`,
   `dw stats` clean. `trace dump` is frozen at the first underrun.
5. Emulator check of the ROM itself:
   `xroar -machine coco2bus -no-extbas -becker -cart-rom carttest
   -no-cart-autorun -type 'EXEC 49152\r'` against `picoco-host`.

Restore `rom load hdbdw3bck.rom` + `save` afterwards.

**Found with it (2026-10-01): back-to-back cart cycles after a Becker
read.** With the firmware as of 2ee4b76 the test failed within seconds to
minutes: the opcode fetched right after a `$FF41`/`$FF42` read came back
as `0x00`, the CPU ran a 6-cycle direct-page instruction in its place
(visible in `trace dump` as a 5-6 us gap after a 2-byte opcode), and from
there either a phantom `$FF42` read or a crash. Cause: core1 ran the read
hooks, trace and counters after the cycle, ~160 clk_sys cycles for a Becker
read against a 167-cycle bus cycle. A CoCo 3 never has a cart cycle straight
after a Becker read (HDB-DOS runs from RAM). Fix in `bus_core1.c`, `bus.c`
and `becker.c`: trace and counters inside the cycle, hooks after the
release, Becker hooks slimmed. Stock HDB-DOS on any CoCo 1/2 would have hit
the same fault. The Plus-W loop still has the old order.

| Check | Result | Date |
|---|---|---|
| XRoar, no ECB | pass: `PASS 0001/0002 SUM D8F6 ERR 0000` against the repo's DINORUN image | 2026-10-01 |
| CoCo 2, firmware 2ee4b76 | fail: 11 clean passes then a desync; later runs failed on the first sector | 2026-10-01 |
| CoCo 2, fixed read path | pass: 55 minutes, `PASS 00D1` (209 passes), `SUM 83CB ERR 0000` throughout. Board: 1.59 G bus cycles, `dw reads 132445` all clean, `becker reads 34302611 underrun 0 overrun 0`, `oe_glitch 0`, `addr_resample 0`, `late_precompute` 181 M (11 % of cycles served from the fresh sample, expected back to back) | 2026-10-01 |

## J. ROM manager (stub + firmware UI)

Spec `docs/superpowers/specs/2026-10-01-rom-manager-design.md`. The manager
is a stub ROM in the firmware (`rom load manager`); the screens are drawn by
the Pico. Never swap the ROM from the USB console while the stub is running.

### J.1 16K CoCo 2, no Extended BASIC

Run 2026-10-01, every row passed (J.2): first screen, arrows, the DOS ROM
refused (`NEEDS EXTENDED BASIC`), carttest launch, one-shot launch, BREAK,
cart power alone, counters, idle session.

### J.3 CoCo 3 (6309, 2 MB) and double RESET

Steps 1-9 run 2026-10-02, every row passed (J.2); the PIO engine re-ran
the HDB-DOS and manager rows in K.2. Open on the 16K CoCo 2 (no Extended
BASIC):

10. RESET twice, `EXEC 49154`: the manager. Passed once on the first
    flash-marker build (3 s window); not re-run with the final 2 s window
    and debounce.

### J.2 Results

| Check | Result | Date |
|---|---|---|
| J.1.1 first screen | pass: `PICOCO  16K NO ECB` (the `$3FFF` mirror check reports 16K on the real machine), list drawn | 2026-10-01 |
| J.1.2 arrows | pass | 2026-10-01 |
| J.1.3 DOS ROM refused | pass: `NEEDS EXTENDED BASIC`, stays in the manager | 2026-10-01 |
| J.1.4 launch carttest | pass: starts with no typing and prints PASS lines; `rom now load carttest.rom`, `rom next load manager`; `dw reads 1712` clean, `underrun 0`, `oe_glitch 0`, `addr_resample 0`. The manager screen is not cleared first, so the pak draws over it | 2026-10-01 |
| J.1.5 one-shot launch | pass: after a power cycle `EXEC 49154` is the manager again | 2026-10-01 |
| J.1.6 BREAK | pass: back to `OK`; `EXEC 49154` re-enters | 2026-10-01 |
| J.1.7 no USB | pass on cart power alone | 2026-10-01 |
| J.1.8 counters | pass: after 35 minutes in the manager on cart power, 16.9 M bus cycles, `underrun 0`, `overrun 0`, `oe_glitch 0`, `addr_resample 0`; `rom now load manager` | 2026-10-01 |
| J.1.9 idle session | pass: highlight still moves after 5 minutes idle | 2026-10-01 |
| J.3.1 HDB-DOS 0.89 MHz, read-path regression | pass (firmware 1.4, Pico 2 PCB): banner, `DIR`, `LOADM"DINORUN":EXEC`; `dw reads 97 crc_err 0`, `becker reads 24929 underrun 0 overrun 0`, `addr_resample 0`, `oe_glitch 0`. `late_precompute 0` (was 4097 per boot: the trace now runs inside the cycle, so the ROM copy is served from the preload) | 2026-10-02 |
| J.3.2 HDB-DOS 1.79 MHz | pass (`hdbdw3bc3.rom`): DINORUN load, three `LOADM"PICOCO:3"`, `SAVE` + `DIR`; `dw reads 199 writes 4 crc_err 0`, `becker reads 51147 underrun 0 overrun 0`, `addr_resample 0`, `oe_glitch 0`, `late_precompute 0` | 2026-10-02 |
| J.3.3 double RESET brings the manager | pass on the CoCo 3: two presses about a second apart, the manager autostarts; log `double reset: manager for this boot`, `rom now load manager`, `rom next` unchanged. The marker is a byte log in flash (neither SRAM nor the POWMAN scratch registers survive a CoCo RESET on this board); window 2 s. Earlier failures were a window of ~0.2 s: the boot log stamps are microseconds, and the Pico releases /HALT ~3 ms after reset | 2026-10-02 |
| J.3.4 arrows | pass: first key presses seen by the stub on a CoCo 3 (manager entered with `rom launch manager` from the console, then `POKE &HFFD8,0:POKE &HFEED,0:POKE 113,0:EXEC &H8C1B`) | 2026-10-02 |
| J.3.5 BREAK | as expected: the screen clears and the manager comes back (a CoCo 3 restart re-enters a `DK` cart). Leaving is by launching a ROM | 2026-10-02 |
| J.3.6 single RESET returns to the default | pass: one RESET from the manager gives HDB-DOS (cold, banner) | 2026-10-02 |
| J.3.7 launch HDB-DOS | pass: ENTER on `HDBDW3BC3.ROM` restarts into HDB-DOS, `DIR` lists the disk. Found on the way: a soft restart in 1.79 MHz mode misses the second byte of the CoCo 3's `DK` check (back-to-back cart reads at 1.79 MHz), so the cart is not recopied; slow speed first fixes it | 2026-10-02 |
| J.3.8 launch carttest (pak, ROM mode) | pass: the cart test runs from the cart on a CoCo 3 (6309) and prints PASS lines; `rom now load carttest.rom`, 30 M bus cycles, `dw reads 2742 crc_err 0`, `becker reads 709389 underrun 0`, `oe_glitch 0`, `addr_resample 0` | 2026-10-02 |
| J.3.9 outside the window / power-on | pass: a RESET 10 s later is a plain warm start (`OK`, no banner); power-on boots have always been HDB-DOS | 2026-10-02 |
| J.3 re-run on 1d86869 (screen clear, RAM size) | pass: header `PICOCO  COCO 3 2M` on the 2 MB machine (RAM probe through MMU slot 4), arrows, `TEMPLE.ROM`/`DAGGORATH.ROM` start on a clean screen, HDB-DOS launch and `DIR` | 2026-10-02 |
| ROM trials from the manager, CoCo 3 | work: Daggorath, Microbes, Temple of ROM, Thexder. Corrupt graphics: Tetris (16K: selects 1.79 MHz with `$FFD9` and runs from the cart, i.e. back-to-back cart cycles in fast mode), Silpheed and Super Pitfall (32K: need A14, and fast mode too) | 2026-10-02 |
| J.3 re-run on the head build (7ea34e6) | pass: double RESET, header `PICOCO  COCO 3`; ENTER on `HDBDW3BC3.ROM` gives HDB-DOS and `DIR`; double RESET, ENTER on `CARTTEST.ROM` prints PASS lines. These are the exits that select slow speed and clear `$FEED` | 2026-10-02 |
| J.3.10 CoCo 2 double RESET | pass once with the first flash-marker build (3 s window): manager loaded, `EXEC 49154`, launch carttest, single RESET back to the saved ROM. Not re-run with the final 2 s window + debounce build | 2026-10-01 |

## K. PIO bus engine

Spec `docs/superpowers/specs/2026-10-02-pio-bus-engine-design.md`; the
engine is described in `docs/firmware-architecture.md` §3.3-3.4 and §8.
Reads are served by PIO and DMA on both boards; the CPU loop is at tag
`fw-1.4-cpu-loop`.

### K.1 Self-test on a bare module (no PCB, no CoCo)

1. `bus selftest` five times: every run ends `selftest fast pass`.
2. `bus selftest restarts` and `bus selftest switches`; Plus-W also
   `bus selftest radio`.
3. `status` after a full run: `engine_stall 1`, `event_lap 1` and Becker
   `underrun` up by 5 are the test's own (a forced stall, a forced lap,
   the empty-port reads). `restarts` stops before all three checks, so
   after it they stay put; `switches` stops after the lap and stall
   checks but before the Becker ones, so `underrun` stays put.
   `stats reset`, then `reboot` to get the saved ROM back.
4. `python3 firmware/tools/bench.py --port <console>` exits 0.

What varies run to run: the restart `zero` counts, the bank-switch counts
and old-bank reads, the hook clk range, the Becker lag max (1-2) and, on a
Plus-W, the 0.89 MHz `+0`/`+1` split. Everything else is the same in every
run.

Pico 2, final build 4a55c93, 2026-10-02. The five identical realistic
bursts are shown once, marked `(x5)`; the sweep rows are elided (they
read `4096` for S=1-21 and `0` from S=22 on at both speeds):

```
fast dma 8-bit write of a7 reaches the TX FIFO as a7a7a7a7
fast engine: PIO0 + DMA, pio0 input_sync_bypass 007fff00: A0-A13 bypassed, R/W bypassed, GP26 synchronised, D0-D7 synchronised; DMA bus priority on (DMA R+W)
fast test: fake 6809 pio1 sm0, dma tx 3 rx 4
bus engine read pio0 sm0, event sm1, dma A 0 B 1 C 2
pio0 gpio_base 0 claimed sm 0 1
pio1 gpio_base 0 claimed sm 0
pio2 gpio_base 0 claimed sm
dma claimed 0 1 2 3 4
fast timing: cycle 84 clk (high 42, low 42), address setup 25 clk, 4096 cycles back to back
fast drop flag: push noblock into a full RX FIFO sets FDEBUG.RXSTALL yes (set before the drop: no)
fast 1.79MHz S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0   (x5)
fast event rate 1.79MHz: counted 20480 of 20480, lag max 0, drop 0, lap 0
fast 1.79MHz mixed r/w S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz gaps r/w/unsel S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz writes r/w/io/unsel S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 3414 lost 0
fast writes: 0 lost, 0 out of order, 0 wrong data of 1366
fast events: 0 mismatches of 512 (the last 512 of 3414 selected cycles); counted 3414 of 3414
fast 1.79MHz restarts 46 under a gaps burst: spurious 0 wrong 0 (zero 58 = cycles lost to a restart); events 3072 of 3072
fast 1.79MHz gaps after restarts S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast events: 0 mismatches of 512 (the last 512 of 4096 selected cycles); counted 4096 of 4096
fast lap: resync 1, next burst events 4096 of 4096
fast stall: detected 1, pins never driven during the stall, next burst 0 mismatches
fast 1.79MHz bank switches 45 under a reads burst S=36: bad 0, wrong bank 0, lost 0; old-bank reads 26, next cycle old 0
fast 1.79MHz bank switches 46 under a gaps burst S=36: bad 0, wrong bank 0, lost 0; old-bank reads 13, next cycle old 0
fast drive off: 0 driven, events 3414 of 3414, writes 0 lost
fast banks: 8/8 banks read their own pattern, 0 mismatches
fast io page: same in 8 banks
fast becker 1.79MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom; underrun 0, overrun 0, 2049 polls in 12293 cycles, fetches bad 0, stalls 0
fast event rate with Becker 1.79MHz: counted 12293 of 12293, lag max 1, drop 0, lap 0
fast becker 0.89MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom; underrun 0, overrun 0, 2049 polls in 12293 cycles, fetches bad 0, stalls 0
fast becker back-to-back: ready -> 02,5c; empty -> 00,ff (stable)
fast becker data then status: the poll right after the last byte's read sees 02 at 1.79MHz, 02 at 0.89MHz (00 = not ready)
fast bank switch 0.89MHz: new bank at +1 cycles (hook 90..90 clk); 512 switches: +0 0, +1 512, +2 0, later 0; bad 0, lost 0
fast bank switch 1.79MHz: new bank at +1 cycles (hook 82..90 clk); 512 switches: +0 0, +1 512, +2 0, later 0; bad 0, lost 0
fast 1.79MHz response_clk 22 (146 ns after OE_BUS fell)
fast 1.79MHz release_clk 6 (D0-D7 low on every cycle 6 clk after OE_BUS rose; -1 = not by 14)
fast 0.89MHz S=72 (480 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 0.89MHz response_clk 22 (146 ns after OE_BUS fell)
selftest rom cleared; reload with rom load
selftest fast pass
```

Pico 2 `bus selftest restarts` (4a55c93), last line before the pass:
`fast restarts: 40 bursts, 1840 restarts, spurious 0 wrong 0 zero 2341, events missing 0`.
`bus selftest switches` (5672ff5, the bank-switch rework), summary line:
`fast switches: 1820 in 40 bursts (S=36), 0 bursts bad; bad 0, wrong bank 0; next cycle old 0`.

Plus-W, final build 0696783, 2026-10-02, shown the same way (the sweep
rows read `4096` below the response point and `0` from it on):

```
fast dma 8-bit write of a7 reaches the TX FIFO as a7a7a7a7
fast engine: PIO0 + DMA, pio0 input_sync_bypass 007fff00: A0-A13 bypassed, R/W bypassed, GP26 synchronised, D0-D7 synchronised; DMA bus priority on (DMA R+W)
fast engine: helper OE_BUS GP40 in pio2 (gpio base 16) synchronised
fast test: fake 6809 pio1 sm0, dma tx 5 rx 6
bus engine read pio0 sm0, event sm1, helper pio2 sm1, dma A 2 B 3 C 4
pio0 gpio_base 0 claimed sm 0 1
pio1 gpio_base 0 claimed sm 0
pio2 gpio_base 16 claimed sm 0 1 2
dma claimed 0 1 2 3 4 5 6
fast timing: cycle 84 clk (E low 42, E high 42), address+selects setup 25 clk to E rise, 4096 cycles back to back
fast drop flag: push noblock into a full RX FIFO sets FDEBUG.RXSTALL yes (set before the drop: no)
fast fake decode delay: E rise -> OE_BUS low 5..5 clk (6 cycles); late /SCS fall -> OE_BUS low 4..4 clk (6 cycles)
fast realistic point: S=40 after E rose = 35 after the fake OE_BUS fell
fast 1.79MHz S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0   (x5)
fast event rate 1.79MHz: counted 20480 of 20480, lag max 0, drop 0, lap 0
fast 1.79MHz mixed r/w S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz gaps r/w/unsel S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz writes r/w/io/unsel S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 3414 lost 0
fast writes: 0 lost, 0 out of order, 0 wrong data of 1366
fast events: 0 mismatches of 512 (the last 512 of 3414 selected cycles); counted 3414 of 3414
fast 1.79MHz restarts 45 under a gaps burst: spurious 0 wrong 0 (zero 62 = cycles lost to a restart); events 3072 of 3072
fast 1.79MHz gaps after restarts S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast events: 0 mismatches of 512 (the last 512 of 4096 selected cycles); counted 4096 of 4096
fast lap: resync 1, next burst events 4096 of 4096
fast stall: detected 1, pins never driven during the stall, next burst 0 mismatches
fast 1.79MHz bank switches 45 under a reads burst S=40: bad 0, wrong bank 0, lost 0; old-bank reads 0, next cycle old 0
fast 1.79MHz bank switches 45 under a gaps burst S=40: bad 0, wrong bank 0, lost 0; old-bank reads 1, next cycle old 0
fast drive off: 0 driven, events 3414 of 3414, writes 0 lost
fast banks: 8/8 banks read their own pattern, 0 mismatches
fast io page: same in 8 banks
fast becker 1.79MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom; underrun 0, overrun 0, 2049 polls in 12293 cycles, fetches bad 0, stalls 0
fast event rate with Becker 1.79MHz: counted 12293 of 12293, lag max 2, drop 0, lap 0
fast becker 0.89MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom; underrun 0, overrun 0, 2049 polls in 12293 cycles, fetches bad 0, stalls 0
fast becker back-to-back: ready -> 02,5c; empty -> 00,ff (stable)
fast becker data then status: the poll right after the last byte's read sees 02 at 1.79MHz, 02 at 0.89MHz (00 = not ready)
fast bank switch 0.89MHz: new bank at +1 cycles (hook 90..91 clk); 512 switches: +0 1, +1 511, +2 0, later 0; bad 0, lost 0
fast bank switch 1.79MHz: new bank at +1 cycles (hook 82..89 clk); 512 switches: +0 0, +1 512, +2 0, later 0; bad 0, lost 0
fast 1.79MHz late /CTS S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 1.79MHz late /CTS + writes S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz late /CTS response_clk 33 after E rose (raw; 220 ns)
fast 1.79MHz late /CTS: select fell at 5; response 28 clk after the select fell, 24 after the fake OE_BUS fell (fake delay 4)
fast 1.79MHz late /SCS S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 1.79MHz late /SCS + writes S=40 (266 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz late /SCS response_clk 33 after E rose (raw; 220 ns)
fast 1.79MHz late /SCS: select fell at 5; response 28 clk after the select fell, 24 after the fake OE_BUS fell (fake delay 4)
fast 1.79MHz response_clk 29 after E rose (raw; 193 ns)
fast 1.79MHz response_clk corrected 24..24 after the fake OE_BUS fell (raw 29 minus the fake delay 5..5); margin to 36: 12..12
fast 1.79MHz release_clk 11 (D0-D7 low on every cycle 11 clk after E fell; -1 = not by 14)
fast 0.89MHz S=77 (513 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 0.89MHz response_clk 29 after E rose (raw; 193 ns)
selftest rom cleared; reload with rom load
selftest fast pass
```

Plus-W (0696783), last lines before each pass:
- `restarts`: `fast restarts: 40 bursts, 1800 restarts, spurious 0 wrong 0 zero 2294, events missing 0`; `status` after it: `start_wait_cap 0`.
- `switches`: `fast switches: 1800 in 40 bursts (S=40), 0 bursts bad; bad 0, wrong bank 0; next cycle old 0`.
- `radio`: `fast 0.89MHz radio: scan active, 2 scans started so far (0 refused, last rc 0), 32 scan results so far, net up`; `net status` after it: `net state up`.
- `stress`: `selftest fast pass` (timing line ends `core0 memcpy stress`), no nonzero mismatch, lost or bad count.
- `bus engine` before and after the runs: identical. pio2 keeps `claimed sm 0 1`, pio0 `0 1`, pio1 none; DMA `0 1 2 3 4`.
- `bus selftest net`: `err bus selftest net: not supported by this engine`.
- `reboot` with `becker net` saved (server `picoco-host` on the Mac): `net up in 4949 ms`, `core1 up, halt released, bus cycles 0`; `status` `cycles 0`, `event_lap 0`.

### K.2 Bench on the PCB

Every row passed on the Pico 2 PCB (K.3); the Plus-W PCB has not run them.
Never run `bus selftest` with the board in a CoCo. Start each session with
`stats reset`; read `status` and `dw stats` after each row.

CoCo 3:

1. `hdbdw3bck.rom` saved, power on: banner, `DIR`,
   `LOADM"DINORUN":EXEC`, a `SAVE` and `DIR` again (0.89 MHz).
2. `rom load hdbdw3bc3.rom`, `save`, power-cycle: the same at 1.79 MHz with
   four `LOADM`s and a `SAVE`.
3. RESET twice within 2 s: the manager. Launch the saved HDB-DOS ROM
   (`DIR` works), then a 16K pak, then `CARTTEST.ROM` (PASS lines, `ERR
   0000`).
4. Double RESET from inside a running pak: the manager again.
5. Tetris from the manager: the graphics display correctly. This is the
   bug that started the engine work (fast mode, back-to-back cart reads).
6. After each row: `bus engine_stall 0`, `event_drop 0`, `event_lap 0`;
   `dw stats` `crc_err 0`; `becker underrun 0 overrun 0`.

CoCo 2 (16K, no Extended BASIC):

7. Power on, `EXEC 49154`: the manager; launch `CARTTEST.ROM`.
8. 30 minutes of PASS lines with `ERR 0000`; then the counters as in 6.

### K.3 Results

| Check | Result | Date |
|---|---|---|
| K.2.1 CoCo 3 HDB-DOS 0.89 MHz, loads + SAVE | PASS: DIR, SAVE, DIR, LOADM DINORUN; dw reads 110 writes 4 crc_err 0; underrun 0 overrun 0 (one LOADM: DINORUN auto-runs) | 2026-10-03 |
| K.2.2 CoCo 3 HDB-DOS 1.79 MHz, 4 LOADMs + SAVE | PASS: DIR, SAVE, DIR, LOADM DINORUN; dw reads 105 writes 4 crc_err 0; underrun 0 overrun 0 (one LOADM, see K.2.1) | 2026-10-03 |
| K.2.3 manager, HDB-DOS / pak / carttest launches | PASS: HDB-DOS DIR; THEXDER (16K) displays; carttest PASS lines, 2665 reads crc_err 0. First double RESET after a console `rom load`+`save` needed a power cycle (once, not reproduced) | 2026-10-03 |
| K.2.4 double RESET from a pak | PASS: from Thexder and from carttest back to the manager | 2026-10-03 |
| K.2.5 Tetris displays correctly | PASS: 151 s, 123.6 M cart cycles at 1.79 MHz, lag max 0 | 2026-10-03 |
| K.2.6 CoCo 3 counters | PASS on every row: engine_stall 0 event_drop 0 event_lap 0 start_wait_cap 0, becker underrun 0 overrun 0, dw crc_err 0 | 2026-10-03 |
| K.2.7 CoCo 2 manager, carttest launch | PASS: EXEC 49154 manager, carttest PASS lines; 1608 reads crc_err 0 at the soak start. (First attempt hit HDB-DOS: the CoCo 3 rows had saved hdbdw3bck.rom; `rom load manager` + `save` restored the default) | 2026-10-03 |
| K.2.8 CoCo 2 carttest 30 min + counters | PASS on f1b4b85 (guard dwell check): 31 min, 866 M cycles, `engine_stall 0`, `dw reads 72195 crc_err 0`, `becker reads 18.7 M underrun 0 overrun 0`, `event_lag_max 1`, `event_dup 75.1 M` (8.7 % of reads: OE_BUS high blips on the first selected cycle after a 6809 internal cycle; never on a `$FF42` read). Soaks 1-2 before the fix: see the dated results below | 2026-10-03 |

## How to resume with Claude

Plug the Pico in, then say "resume the bench test plan at section K.2" (or
any open row). Claude will check the console port, print `status`, and walk
the steps.

Step 0 on resume: check what is running (`version`, `status`) before
flashing anything. To reflash: `bootsel` on the console, wait for
`/Volumes/RP2350`, then `cp -X build-pico/picoco.uf2 /Volumes/RP2350/`
(rebuild with `ninja -C build-pico` if the UF2 is older than the last
commit; `picotool load -x` if the drive won't mount). Results get appended
to this file under a dated "Results" heading.

## Results

### 2026-10-03, PCB v2.3.1 #1, Pico 2, CoCo 2 (26-3026, 16K, no ECB): PIO engine stall guard
carttest soaks 1 and 2 (7bec09f, 6fa526c) died after 7-13 PASS lines with
`engine_stall` 1-3. Logged at detection (6fa526c): SM at `pull`, RX 0, TX 0,
DMA B idle, OE_BUS high, 486 cycles served correctly between the two ticks,
frozen trace normal. A guard false positive: a short OE_BUS high blip near
the end of a cycle makes the read SM re-push and pass through `pull` after
the cycle; the restart (stop, discharge, start) crashed the CoCo (the
cassette relay clicks when the crashed CPU reaches BASIC's PIA init). The
trace shows ~7-9 % duplicate read events, each on the first selected cycle
after a 6809 internal cycle (CLRA, TSTA, DECB, LEAX, JSR, ANDCC). The old
CPU loop never saw them (`oe_glitch 0`, sampled every ~25 clk). Fix
f1b4b85: a tick counts only after 32 consecutive PC reads at `pull`;
`event_dup` counts the blips. Soak 3: 31 min clean (K.2.8). The CoCo 3
never shows a blip or a stall. Open: the blips' origin (scope OE_BUS on the
cycle after a CLRA), whether to filter them in PIO, and a bare-module
`bus selftest` on f1b4b85 (the forced-stall line with the dwell check).


### 2026-09-30, PCB v2.3.1 #1, Pico 2, CoCo 3 (firmware 1.3 + core1 timing fix)
First assembled PCB (docs/pcb-bringup.md has the phase-by-phase log). HDB-DOS
boots on cart power alone, CoCo RESET reboots the Pico through U13/R9, DIR,
LOADM, SAVE and the manager all pass, `addr_resample 0` (the breadboard
needed the resample on every boot). hdbdw3bc3 (1.79 MHz) failed
intermittently with a one-byte Becker desync; root cause and fix in
docs/pcb-bringup.md "1.79 MHz fault". After the fix: 4 LOADMs + SAVE at
1.79 MHz, `dw reads 106 writes 4 crc_err 0`, `becker underrun 0`. Trace now
freezes at the first underrun/CRC error (`trace run` thaws).


### 2026-09-29, breadboard: bridge mode, latency, clock, CoCo manager (firmware 1.2)
Firmware 1.2 (branch coco-manager) on the Pico 2 breadboard rig, CoCo 3.
- **Bridge mode (breadboard step 10) passes** against two servers: `picoco-host`
  over `firmware/tools/becker_relay.py` (CDC0 to TCP), and DriveWire 4.3.6p
  (Maven build, `DriveWireServer --noui`, serial device on CDC0, `SerialDTR`
  true, `HDBDOSMode` true). DIR, LOADM+EXEC of DINORUN, SAVE, DIR, all
  correct; `becker underrun 0 overrun 0` over 76 k Becker reads. Timings with
  no added delay: DIR 1 KB in 0.10 s, LOADM 23 KB (90 sectors) in 2.16 s.
- **Latency limit (WiFi stand-in, relay `--delay/--jitter`):** 25 ms one-way
  is fine (DIR 0.59 s, LOADM 14.3 s: two round trips per sector, purely
  latency-bound). 100 ms one-way is fine (boot's 3 sectors 1.17 s, DIR 1.64 s).
  150 ms one-way hangs the CoCo at the HDB-DOS banner: the server's
  `DW_PAYLOAD_TIMEOUT_MS` (250 ms) expires before the CoCo's READEX checksum
  bytes arrive (~305 ms after the data went out), the op is dropped, no status
  byte is sent, and the CoCo's Becker loop waits forever (`timeouts=1` in
  picoco-host stats). DW4 has the same class of limit (`ReadByteWait`, 200 ms).
  So a remote server needs a round trip under ~200 ms; the Pico is not the
  limit.
- **Clock (plan Task 8):** `pico_aon_timer` keeps time across a watchdog
  `reboot` and a BOOTSEL reflash (`clock kept`). A CoCo reset or power-cycle
  does not reboot the Pico on the breadboard (no RUN tie); the RUN-pin path
  is a PCB check.
- **CoCo manager (plan Task 15):** `DRIVE 3:RUN"PICOCO"` runs; list, jump,
  mount, already-mounted message, eject, new image, settings (ROM pick,
  HDB-DOS toggle, clock set, bad date), save, DIR after exit, boot picker
  and HELLO.BAS all pass; `reply_overflow 0`, `becker overrun 0`;
  `dw mount 1 picoco.cfg` and `dw disk insert 1 picoco.cfg` refused.
  Found: the SHIFT+letter chords never arrived as lowercase on the real
  CoCo 3 (XRoar's `-type` injects ASCII, so it never exercised a real
  SHIFT). Rebound to plain letters with `G` for goto. Also: mounting an
  image already open read-write in another drive fails on FatFS
  (`FF_FS_LOCK`); the firmware now reports `already mounted`.

### 2026-09-16, HDB-DOS boot + native DriveWire on the CoCo 3
Root cause of the 09-15 HDB-DOS garbage: core1 sampled the address ~20 ns
after OE_BUS fell and A8/A10 (sometimes A0/A2) read high on ~3% of cart
reads (trace: single-sample spikes of 0x100/0x400, steady rate, not a
loose wire). The CoCo 3 copies the cart into RAM at boot, so HDB-DOS ran
corrupted. Fix: `bus_core1.c` reads the GPIOs twice ~70 ns apart and uses
the second; `status` prints `bus addr_resample N bits X` (first-sample
disagreements). After the fix: 0 spikes in a 4096-read sweep, HDB-DOS 1.5
BECKER (`hdbdw3bck.rom`) boots, `DIR` of DINORUN.DSK on drive 0 (`dw
hdbdos on`) lists correctly, `LOADM"DINORUN":EXEC` loads and runs: `dw
reads 97 crc_err 0 timeouts 0`, `becker reads 24929 writes 679 underrun 0`.
Boot still logs addr_resample hits on all bits during the ROM copy (fast
loop); revisit before the `bc3` 1.79 MHz ROM. `hdbdw3bc3.rom` (CoCo 3 build, 1.79 MHz during transfers) also passes: boot,
DIR, LOADM+EXEC of DINORUN: `dw reads 97 crc_err 0`, Becker byte period
18 us (vs 37 us with the bck build), `addr_resample 97 bits 3ffe` for the
whole boot+load (vs 2629 with bck), so the 70 ns settle has margin at speed.
Saved config now serves bc3.
Step 11 write half also passed: `SAVE"HELLO"`, CoCo power-cycle, `DIR` still
lists it: `dw writes 4 write_err 0 crc_err 0`. Caveat: `POKE &HFFDE,0` ROM mode on the CoCo 3 with a cart
present hides Super ECB behind the cart window; BASIC then executes 0xFF
from the Pico (?NF ERROR, runaway). Use only for a lone PEEK, or not at all.

### 2026-09-15, breadboard gates 9 and 10 on a CoCo 3
Address buffers + 74HC00 decode (bench guide gate 9): passed after U15 pin 14
was found on GND. A CoCo 3 runs BASIC from RAM, so `PEEK(&HC123)` never hits
the cart; the boot-time `0000 R`/`0001 R` and /SCS PEEKs ($FF41/44/48/50)
proved A0-A4, A6-A13. Data buffer U10 + Becker loop (gate 10, plan step 9):
`POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` -> `0 2 65`,
trace `3f42 W 41`, `W a5` echoed exactly, `becker reads 3 writes 3 underrun
0`. First attempt traced `W e1` with the screen still reading 65: GP2-GP7 were
one Pico pin low (pin 3 GND not skipped). A5 still unproven.

### 2026-09-08 evening, reflash
`main` 78fb594 flashed via BOOTSEL; `version 0.3-pico`, config replayed
(`mode native`, `bus drive off`, `dw hdbdos off`), `dw selftest` ok.

### 2026-09-08, host validation against a real DW4 disk image
`picoco-host` with `dw/drivewire4/disks/run-dino-run/DINORUN.dsk` (161280
bytes, RAW) on drive 0 and a scratch copy on drive 1; `tools/dwtest.py`
in both `--hdbdos off` and `--hdbdos on`: all 7 checks ok (630/630
sectors match, READ checksum, READEX corrupt → E_CRC, unmounted →
E_NOTRDY, scratch write/readback/corrupt-write, TIME).
