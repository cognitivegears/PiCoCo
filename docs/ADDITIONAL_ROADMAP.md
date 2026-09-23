# PiCoCo — additional roadmap ideas

Status: brainstorm, 2026-09-08. Companion to `RP2350B_IDEAS.md`; that doc
covers what extra GPIO unlocks, this one collects everything else. Nothing
here is committed to the MVP (DriveWire over Becker + ROM emulation).

---

## 1. PiCoCo-DOS: a small open-source boot ROM we can ship

**Problem.** PiCoCo is useless out of the box: the CoCo needs a DOS ROM
in the /CTS window and every existing one (HDB-DOS, SDC-DOS, RGB-DOS,
ADOS) is a patch on Microsoft's Disk Extended Color BASIC, which nobody
can license. We don't check those binaries in (see §3), so a fresh
board boots to a blinking cursor until the user finds a ROM.

**Idea.** Write a small, 100% original 6809 ROM, MIT-licensed, that makes
the board work for the common cases with zero third-party files. Most
people will replace it with HDB-DOS or SDC-DOS within the hour; that is
fine. Its job is "plug in, `DIR`, `LOADM"GAME"`, `DOS`, done."

**Mechanism.** ECB is designed to be extended from a cartridge: it looks
for `DK` at $C000 on reset, jumps to $C002, and exposes the RAM hooks at
$015E..$01A8 for intercepting tokenizing, LIST, OPEN, errors, etc. Disk
BASIC bolts itself on exactly this way. No patching of the internal ROM,
no DMA, no CoCo 3 ROM-in-RAM tricks.

### Must have (v1)

| Item | Why |
|---|---|
| `DK` signature, $C002 init, RAM hooks | ECB cartridge protocol |
| DSKCON vector at [$C004], variable block pointer at [$C006] | ML programs do `JSR [$C004]` for raw sector I/O |
| DSKCON variables at $EA..$F0 (DCOPC/DCDRV/DCTRK/DCSEC/DCBPT/DCSTA), same op codes (0 restore, 2 read, 3 write) and status bits | same reason; programs PEEK/POKE these |
| Memory map: $0600..$0DFF reserved, program start $2600, PCLEAR behaviour | LOADM'd binaries assume it |
| RS-DOS filesystem reader: directory track 17, granule map, 18 sectors × 256 | needed by DIR/LOAD/LOADM |
| `DIR`, `DRIVE n`, `LOAD`, `LOADM` (+ EXEC address), `RUN "file"` | the 90% case |
| `DOS` | NitrOS-9 boot; kernel never calls BASIC afterward |
| Disk BASIC token numbers for every command/function, implemented or not | tokenized .BAS files must LOAD and LIST correctly; unimplemented ones raise ?FC |
| Disk BASIC error codes (?NE, ?IO, ?FM, ?FS ...) | programs `ON ERR` on them |
| Backend: DriveWire OP_READEX/OP_WRITE over Becker; SD images once SD lands | already have the transport |

### Nice to have

- `SAVE`, `SAVEM`, `KILL`, `RENAME`, `DSKI$`/`DSKO$`, `FREE`.
- Sequential file I/O (`OPEN "I"/"O"`, `INPUT#`, `PRINT#`, `LINE INPUT#`,
  `EOF`, `CLOSE`). This is where the size and effort start to climb.
- A one-screen boot menu (pick image, drive mapping) driven by the same
  console commands `picoco.cfg` uses.
- DriveWire virtual-channel bits (`DWLOAD`-style loader) if trivial.

### Non-goals, stated so nobody re-argues them

- Random files: `FIELD`/`GET`/`PUT`/`LSET`/`RSET`/`CVN`/`MKN$`. Replace
  the ROM if you need them.
- `BACKUP`, `COPY`, `DSKINI`, `VERIFY`, `MERGE`.
- Absolute-address compatibility with Disk BASIC internals. 80s copy
  programs, disk editors and protected loaders that call into the middle
  of the 8 KB will break. HDB-DOS keeps Microsoft's layout precisely for
  those; we can't without copying it.
- HDB-DOS's internal DriveWire entry points that `DW*` utilities call.

### Legal position

Clean room: written from the behavioural spec (the Unravelled series,
Disk BASIC manual, DriveWire spec), never from a disassembly listing.
Token numbers, vector addresses, DP variable locations and the memory
map are functional interfaces, same footing as the SDC register map in
§2. Ship source and the built `.rom` in the repo under MIT; document
"replace with HDB-DOS from Cloud-9/ToolShed for full Disk BASIC".

### Sizing

- Budget 2–4 KB of the 8 KB window for v1; sequential file I/O maybe
  +2 KB. Leaves room for the menu and a Becker client.
- Toolchain: `lwasm` from lwtools (not yet in the repo; add under
  `rom/` with a Makefile and a host-side smoke test that loads the ROM
  into a 6809 emulator core, or at minimum asserts the `DK` header and
  vector table).
- Effort: weeks part-time for must-haves. Sequential I/O roughly
  doubles it.
- Depends on: Becker/DriveWire path (done). SD image backend is a v2
  item.

---

## 2. CoCo SDC command-interface emulation

`RP2350B_IDEAS.md` §2 already picks this over a bare WD1793 and §12.1
maps the address conflicts. This section records what we learned about
the interface and the ground rules for implementing it.

**Interface is public; hardware and firmware are not.** Darren
Atkinson's *CoCo SDC Programmer's Guide* documents the control latch
($FF40, write $43 for command mode), flash data/control ($FF42/$FF43),
command/status ($FF48) and the three parameter/reply registers
($FF49..$FF4B), plus the extended commands that take a 256-byte block
(mount image, new image, set/list directory, drive info). The AVR
firmware and PCB were never released and the board is still sold
(BoysonTech, Retro Rewind). We need neither.

**Open reference implementations of the host side:**

- NitrOS-9 `llcocosdc.dr` (Boisy Pitre) — exact polling sequence.
- `n6il/cocosdc-commander` — exercises the extended commands from
  DECB/OS-9/FLEX. Check its license before lifting anything.
- `nowhereman999/CoCoSDC_StreamFile` — streaming mode.
- VCC's SDC simulator — device side, but **GPLv3**; read for behaviour
  only, never copy into our MIT firmware.

**Fit.** All registers are under /SCS in $FF40..$FF4B, so `bus_table`
read hooks cover it, pure polling, no /HALT or /NMI. The flash data
register at $FF42 collides with Becker data: profile-switch or move
Becker. The SDC's eight 16 KB flash banks map onto our /CTS window,
which is where SDC-DOS's ROM-bank features come from.

**Ground rules.**

1. Implement from the Programmer's Guide and the open host-side code.
   Never dump or disassemble the SDC's AVR.
2. Don't bundle SDC-DOS (Darren's patch of Disk BASIC, see §3).
3. Don't put "SDC" in the product name. "Compatible with the CoCo SDC
   command interface, runs SDC-DOS" is descriptive use. Credit Darren.
4. Courtesy heads-up to Darren Atkinson and Ed Snider before shipping.
   Not required; cheap.

---

## 3. ROM distribution policy (decided 2026-09-08)

- **Never check in HDB-DOS, SDC-DOS or Disk BASIC binaries.** All are
  Microsoft/Tandy Disk Extended Color BASIC with community patches.
  HDB-DOS source is public in ToolShed's `hdbdos/` tree and Cloud-9
  distributes it freely, but they cannot license the Microsoft core; it
  is source-available, not open source. Same for SDC-DOS.
- Users fetch HDB-DOS from Cloud-9 or build it from ToolShed. That is
  exactly what DriveWire itself does. Document the one-line build.
- **Considered and rejected: shipping only a delta patch** (IPS-style
  offset/byte records applied to a user-supplied Disk BASIC 1.1 image at
  boot, in the Pico's ROM table, no DMA needed). Legally cleaner, and
  trivial to implement, but the user still has to source `disk11.rom`,
  so they may as well fetch HDB-DOS. Not worth the extra step. Revisit
  only if a rights-holder ever objects to the ROM images themselves.
- Considered and rejected: patching Extended BASIC. Not needed; the
  cartridge hook protocol is the extension mechanism (§1).
- If we ever make a PiCoCo flavour of HDB-DOS, publish it as source
  patches the way ToolShed does, not as a ROM image.

---

## 4. Suggested order

1. Ship MVP with "bring your own HDB-DOS".
2. PiCoCo-DOS must-haves (§1). Unblocks "works out of the box".
3. SDC interface (§2) once SD storage exists, since its whole value is
   mounting images from a card.
4. PiCoCo-DOS sequential file I/O only if people ask.
5. Bridge mode (§5) when a host-side DW4 feature is actually wanted.

## 5. Bridge mode: Becker port to a host DriveWire server (deferred 2026-09-17)

Breadboard plan step 10. The firmware already has `becker bridge`, which
forwards Becker bytes to CDC0 so a DriveWire server on the Mac
(pyDriveWire or DW4) serves the CoCo. It was skipped on the bench
because native mode (step 11) passed first: DIR, LOADM+EXEC, SAVE across
a power-cycle, at both 0.89 and 1.79 MHz, crc_err 0.

Why it is still worth finishing later:
- Access to everything a full DW4 server does that the Pico does not:
  virtual serial ports, printer capture, network drives, the DW4 UI.
- A fallback when a disk image or a client (NitrOS-9, HDB-DOS variants)
  trips a gap in the native server: same cart, no reflash.

What it needs: `becker bridge` + `save`, a CDC0 session from
pyDriveWire (`--port /dev/tty.usbmodemXXXX1 --speed 115200 <image>`),
then the step 10 checks (`DIR`, `LOADM`). Tag `fw-0.8-bridge` when it
passes. Watch for the host-side latency: the CoCo's Becker read loop has
no timeout, so a slow reply hangs the CoCo until the server answers.


## 6. Backlog from the 2026-09-19 quality reviews (firmware + docs, no hardware impact)

Sources: `.superpowers/sdd/2026-09-17-main-board-v2.3/quality-ee.md`, `quality-coco.md`,
`quality-fw.md`. Hardware items from those reviews were folded into v2.3.1 before the
first order (ground stitching, 0.5 mm power trunks, C16, sound values, JP5, J1,
wider antenna keepout) or deferred to v2.4 (see hardware-design §9).

### Firmware, ordered by value
1. **DONE (2026-09-22).** FAT safety across CoCo resets: `dw_server.c`'s `do_write()`
   calls `ops->sync` after a successful DriveWire write (and reports the write as failed
   if the sync itself fails), and `fs_flash_write_blocks` skips the erase/program cycle
   for any 4 KB block that's unchanged (the common case: a sync after every write
   re-touches the same root-dir/FAT block). `n_fat` stays 1 — on this flash both FAT
   copies would share one 4 KB erase block and FatFS never reads FAT2 on mount, so a
   second FAT would only double FAT erases with no real protection.
2. **Auto-save mounts.** `dw mount` reaches flash only on `save`; the community workflow is
   mount-then-reset-to-boot, so the mount is lost. Write `picoco.cfg` on mount/eject.
3. **DriveWire virtual-channel command shell.** `dw_server.c` stubs OP_SERWRITE/OP_SERREAD.
   DW4's server shell rides that channel and is how NitrOS-9's `dw` utility inserts disks.
   A minimal `dw disk show|insert|eject` gives disk selection from the CoCo with client
   software that already exists. Worth more than PiCoCo-DOS (§1).
4. **NitrOS-9 over Becker as a passed milestone** (EOU `dwio_becker.sb`, boot `/dd` from
   HDB-DOS `DOS`). DWINIT already clears `hdbdos` for drive numbers < 0x80.
5. **DONE (2026-09-22).** Write-cycle sampling: `bus_core1.c`'s write path now keeps the
   last `gpio_in` sample taken while OE_BUS was still low (`prev`) and uses its data bits,
   instead of the first sample with OE_BUS high.
6. **Read-path margin.** Ten nops (67 ns at 150 MHz) before the address read; real
   OE-to-data ~180-200 ns, not the documented 70 ns. On the first PCB read `bus
   addr_resample`; if zero, drop the nops. Then `set_sys_clock_khz(200000)`.
7. **/HALT flow-control holds** longer than a GIME tick (16.7 ms) cost NitrOS-9 clock ticks;
   bound runtime holds to a few hundred us (the 1.2 s boot hold is fine).
8. **DONE (2026-09-22).** Flash-free core1 build check: `firmware/tools/check_core1_flash_free.py`
   runs as a POST_BUILD step on the `picoco` target, following every direct branch from
   `bus_core1_main` and the Becker read hooks, and fails the build if any lands outside SRAM.
   `bus_core1.c` still has no test coverage; see TEST_PLAN.md.
9. **DONE (2026-09-22).** `PICOCO_FS_OFFSET`/`PICOCO_FS_SIZE` moved to the board headers
   (`fs_flash.h` includes `PICOCO_BOARD_H`); the Plus-W gets the rest of its 16 MB flash
   (3712 FAT12 clusters). The Plus-W board header trap avoided: `firmware/boards/plusw.h`
   defines no `PIN_LED` (its LED pin is unverified), so LED code compiles out on that board.
10. SPDX headers on every firmware source; `console_exec` should reject >6 tokens / >135
    chars instead of truncating silently; note that `bus_stats` counters are non-atomic.
11. Sound firmware (PWM on GP34/header 34) does not exist yet; the analog stage is populated.
12. A firmware-pulsed /CART (JP5 1-2 on a Pico 2, GP33 on a Plus-W): assert for the first
    cycles after reset for autostart ROM images, release for DK-signature DOS ROMs.

### Docs
- README: on a CoCo 3 a BASIC `PEEK(&HC000)` never reaches the cart, use $FF41/$FF42; the
  MPI slot register ghosts at $FF9F as well as $FF7F; JP4 bridged breaks HDB-DOS (it jumps
  to $C000 as code); a dead or blank module holds /HALT and looks like a dead CoCo; the
  DNP stages are Plus-W provisions, not indecision; one-line ToolShed build for HDB-DOS;
  credit the DriveWire and HDB-DOS authors; the bare 98 x 77 board fits no Program Pak
  shell (the 53.34 x 44.45 mm roadmap outline is the cased version).
- firmware-architecture: the macro is `PICO_FLASH_ASSUME_CORE1_SAFE` (not `PICOCO_`); the
  ROM window is $C000-$FEFF (rom.c covers idx 0x0000-0x3EFF), not $C000-$DFFF; the "70 ns"
  latency claim is stale; the GP25 trap above.
- hardware-design: §3.1 must not list /HALT, /NMI, /CART as U13 inputs; §4.6 sound numbers
  recomputed for the loaded network; JP3 1-2 ties Plus-W GP34 to GP42 (keep one an input);
  §1: the HDMI corner is not wirable on v2.3 (HSTX = GP12-19 = A4-A11), it is reserved for a
  variant that moves the address bus.

### Hardware deferred to v2.4
- 8 x 33 R series termination on D0-D7_CART (R11/R12 terminate the two quietest nets while
  the data bus has none); no room without a re-place.
- /SLENB drive for Multi-Pak writes (CocoFLASH asserts SLENB on cart-space writes because
  the MPI data buffer blocks them): U15 spare gate as an inverter on OE_BUS into a 2N7002,
  behind a jumper. Unverified; needs a bench MPI first.
- C3 as a 10 V 1206 (a 22 uF 6.3 V X5R 0805 at 3.3 V bias delivers about half its value).
- Real 2.4 GHz clearance (5 mm) around the Plus-W antenna, not a 1-2 mm rule area.
