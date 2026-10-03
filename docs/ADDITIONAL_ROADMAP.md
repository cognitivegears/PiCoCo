# PiCoCo — additional roadmap ideas

Open work beyond what is built and bench-passed (DriveWire native and bridge
modes, WiFi transport on a Plus-W, NitrOS-9/EOU, the ROM manager, the PIO bus
engine on CoCo 2 and CoCo 3). Companion to `RP2350B_IDEAS.md`, which covers
what the Plus-W's extra GPIO unlocks; this doc collects everything else.
Done items are removed, not marked; `firmware/TEST_PLAN.md` holds the record.
Last pruned 2026-10-03.

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
- The ROM manager (`coco/stub/manager.asm`, `firmware/src/ui/`) already
  covers image selection and launching; PiCoCo-DOS is the piece that makes
  `DIR`/`LOADM` work without a Microsoft-derived ROM. SD image backend is a
  v2 item.

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

1. A14/A15 on every build (§8): the only item that is a correction, not a
   feature.
2. PiCoCo-DOS must-haves (§1). Unblocks "works out of the box".
3. SDC interface (§2) once SD storage exists, since its whole value is
   mounting images from a card.
4. PiCoCo-DOS sequential file I/O only if people ask.

## 5. Firmware backlog and deferred hardware

Started from the 2026-09-19 quality reviews (`.superpowers/sdd/2026-09-17-main-board-v2.3/
quality-*.md`); hardware items from those went into v2.3.1 or v2.4 (hardware-design §9).

### Firmware, ordered by value
1. **Auto-save mounts.** `dw mount` reaches flash only on `save`; the community workflow is
   mount-then-reset-to-boot, so the mount is lost. Write `picoco.cfg` on mount/eject.
2. **/HALT flow-control holds** longer than a GIME tick (16.7 ms) cost NitrOS-9 clock ticks;
   bound runtime holds to a few hundred us (the 1.2 s boot hold is fine).
3. SPDX headers on every firmware source; `console_exec` should reject >6 tokens / >135
   chars instead of truncating silently; note that `bus_stats` counters are non-atomic
   (`stats reset` races core1's own counters, firmware/README.md).
4. Sound firmware (PWM on GP34/header 34) does not exist yet; the analog stage is populated.
   Specs: `docs/superpowers/specs/2026-09-27-*` (bus-engine spec first, now done).
5. **Firmware-pulsed /CART, bench check.** Built for a Plus-W's `CART_DRV` pad (GP33):
   toggled ~500 Hz for 500 ms after the `/HALT` release when `rom_cart_wanted()` (`cart
   auto`: a loaded ROM whose first two bytes are not `DK`; `cart on`/`off` override and are
   what `save` writes). No Pico 2 board header defines `PIN_CART_DRV`, so `cart on`/`off`
   are refused there; the JP5-on-a-Pico-2 path is unbuilt. Still open: a GMC image
   autostarts on a CoCo 3 with `cart auto`, and HDB-DOS still reaches BASIC normally (the
   pulse must not disturb a DOS boot). The self-test does not exercise `/CART`.
6. **Make EOU easy for a newcomer.** (a) prebuilt `picoco-host` binaries for macOS, Linux
   and Windows, so nobody needs CMake and a compiler to serve an image (a Windows build has
   never been tried); (b) one launcher command that starts the server and
   `becker_relay.py --reconnect` together and finds the bridge port by itself. Before
   announcing either: build and boot the 6809 EOU image (only the 6309 one has run), and
   ship `eou_becker.py`, not a remastered image, unless the EOU project's terms allow
   redistributing one.
7. **PIO decode of `$FF40`.** A bank switch is core1's `$FF40` write hook, which lands
   82-93 clk after the write ends, after the next cycle has taken its bank: the first cart
   fetch after a `STA $FF40` comes from the old bank at both speeds
   (firmware-architecture §3.2.4; convention for banked images in `coco/README.md`).
   Harmless unless a banked pak switches from code in the switched window. If one needs
   it, decode `$FF40` writes in PIO and set the bank register there, with no core1 in the
   path, and a sequence bit (§6) so two identical writes are two switches. Note PIO has no
   register transfer between state machines, so the bank's route into the read SM's X is
   the design problem; spike first.
8. **CoCo 2 OE_BUS blips** (bench 2026-10-03, TEST_PLAN K). On the bench CoCo 2 (26-3026)
   ~9 % of reads show an OE_BUS high blip inside the cycle, always the first selected cycle
   after a 6809 internal cycle; the CoCo 3 shows none. The engine survives them (the stall
   guard needs a parked SM; a blip only splits the event, `status` `event_dup`), but each
   one re-serves the byte ~25 clk after the blip. Scope OE_BUS on the cycle after a `CLRA`
   before choosing: an end-of-cycle filter in the event SM (cheap), a PIO1 helper
   debouncing the read SM's end like the Plus-W's (~3 clk of Pico 2 margin), or an RC on
   OE_BUS at the respin (100 pF behind R11's 33 R is nothing; ~470 pF costs 5-7 clk).
9. **Plus-W PCB on a CoCo.** The engine has only run on a bare Plus-W (self-test). TEST_PLAN
   G.2 and K.2 rows for the Plus-W board are open.
10. `version` still prints 1.4; bump with the next firmware tag.

### Hardware deferred to v2.4
- **CoCo /RESET drive** (from §6): one more 2N7002 + gate resistor on cart pin 5, like
  Q2 on /HALT, so firmware can cold-boot the CoCo into a newly selected ROM. Today the
  Pico can only *be* reset by /RESET (RESET_BUF -> RUN); it cannot assert it. Pairs with
  the JP5 /CART pulse (§5 item 5) to give "pick a ROM, machine reboots into it". Double
  RESET into the manager covers "back to the menu" already.

### Long term (v3)
- **Evaluate dropping U11, U12 and U13** (the three input-only LVC245s on address and
  control). §6: RP2350 GP0-25 are 5 V tolerant with IOVDD powered, and LVC00 inputs
  are 5 V tolerant, so A0-A13, R/W, CTS, SCS, E and RESET could go straight to the
  Pico and U15. Before deciding, check: (a) what a powered CoCo does to an unpowered
  Pico (VSYS is diode-fed from cart 5 V, so IOVDD rises with the bus; the window is
  the regulator ramp only); (b) edge quality and address settling on a scope (the PIO engine has no resample counter) on the
  buffered PCB vs a bufferless breadboard; (c) the Plus-W, where RESET_BUF drives
  RUN and Q/SLENB reach the pad grid; (d) whether the freed board area pays for the
  8 x 33 R data-bus termination above. U10 and U15 are not candidates.
- 8 x 33 R series termination on D0-D7_CART (R11/R12 terminate the two quietest nets while
  the data bus has none); no room without a re-place.
- /SLENB drive for Multi-Pak writes (CocoFLASH asserts SLENB on cart-space writes because
  the MPI data buffer blocks them): U15 spare gate as an inverter on OE_BUS into a 2N7002,
  behind a jumper. Unverified; needs a bench MPI first.
- C3 as a 10 V 1206 (a 22 uF 6.3 V X5R 0805 at 3.3 V bias delivers about half its value).
- Real 2.4 GHz clearance (5 mm) around the Plus-W antenna, not a 1-2 mm rule area.

## 6. Learnings from sbelectronics multicart/videocart (reviewed 2026-09-28)

Scott Baker's <https://github.com/sbelectronics/coco/tree/master/multicart-videocart>:
a Pico 2 W ROM multicart (OLED + encoder menu, WiFi web UI/OTA, banked ROMs to 128 KB,
cold-boot into the selected ROM) and a videocart variant that snoops every bus write
into a RAM mirror and renders VDG modes over HSTX to HDMI. CoCo 1/2, no disk or
DriveWire, Apache 2.0, through-hole, months of use. Read its `HOW-IT-WORKS.md` before
touching `bus_core1.c` again; the timing census there is the best public one.

### Decisions confirmed
- **Keep the buffered bus (U10 + U15).** He proved RP2350 GP0-25 5 V input tolerance is
  real and usable, so U11/U12/U13 (address/control *in*) are belt and braces: LVC Ioff
  and clean edges, nothing more. A cost-down v3 could feed CTS/SCS/E straight into
  U15 (LVC00 inputs are 5 V tolerant) and drop them. U10/U15 stay for reasons he does
  not have: we take Becker *writes* (U10 DIR follows R/W in hardware, so a wrong
  pindir never reaches the 5 V bus), hardware /OE gated on E releases the bus even if
  core1 is late (his safety depends on a 224 ns lap that does nothing else; ours runs
  hooks and will run sound), and 24 mA drive vs his 8 mA. Neither design puts 5 V
  levels on the bus; both drive 3.3 V into TTL thresholds.
- **No overclock by default.** He needs 250 MHz because his loop is continuous-refresh
  polling: worst-case detect latency is a full lap (374 ns at 150 MHz vs a ~318 ns
  budget, 85 % hit rate, exposed behind an MPI). Ours spins on the OE_BUS edge, so
  detect latency is tens of ns. The PIO engine serves in 22 clk (146 ns) at 150 MHz
  with 14 clk of margin at 1.79 MHz; the clock stays the last lever (sound PWM, USB and
  flash dividers assume 150 MHz and the RP2350 is rated for 150).

### Numbers to design against (his measurements, CoCo 1/2, 1117 ns cycle)
- CoCo latches read data ~478 ns after !CTS asserts; his loop drives ~160 ns after
  the sample that sees !CTS low.
- An MPI adds ~40 ns to !CTS through its '367 and '139. For us that lands on OE_BUS
  via U12 -> U15 (~50 ns total) and comes straight out of the 280 ns CoCo 3 fast
  window. CoCo 3 + MPI + 1.79 MHz is our worst case (TEST_PLAN F.4).
- 6809E data setup before E falls: ~80 ns at 0.89 MHz; the write data is valid from
  ~E-rise + 225 ns.

### Rules applied in the PIO bus engine
- **Nothing on core1 touches the APB per event.** His banked ROMs glitched whenever the
  radio was on: the hot loop polled a PIO RX FIFO, CYW43 DMA bursts stalled it, and the
  CoCo latched mid-transition. Our engine serves reads with no core in the path and
  core1 reads events from an SRAM ring that DMA C fills as the FIFO's only consumer.
- Erratum E9 (reset pull-down on every pad): the read SM drives D0-D7 low before every
  release; R7 went 100 k -> 10 k.
- A change-driven consumer needs a sequence bit: two identical back-to-back writes
  are otherwise one. Relevant to item 7 of §5.

### Worth adopting
- `tools/bas2rom.py` (Apache 2.0, stdlib Python): .cas/.bas/.bin -> autostart ROM,
  banked layouts for >16 KB, one ROM runs on CoCo 1/2/3. Works with any ROM cart.
- The 6809 warm-start stub (clears BASIC's `$0071` flag for one boot so a selected ROM
  gets a true cold start) and "insertion-style" start (stay invisible until READY,
  then raise FIRQ) for cassette conversions. Both need the /RESET drive above.
- His host-side `make audit` is the same idea as `check_core1_flash_free.py`;
  his `make test` suite includes a sampler-timing test that recomputes PIO dividers
  from the clock constant, which is what we would want before any clock change.

### Not worth chasing
- HDMI video: his renderer is VDG-only, whole-frame, CoCo 1/2, and cannot sit behind
  an MPI. Our HDMI corner stays a keepout for a variant that moves the address bus.
- OLED + encoder: PICOCO.BIN on the CoCo and the USB console cover selection; WiFi
  (Plus-W) is the eventual remote UI.

## 7. ROM manager follow-ups (from the 2026-10-01 design)

Spec: `docs/superpowers/specs/2026-10-01-rom-manager-design.md` section 10.

- **Firmware-pulsed `/CART` autostart** for machines without Extended
  BASIC, so the manager (or a Program Pak set as default) starts with no
  `EXEC` typed. Needs JP5 at 1-2 on a Pico 2 (never with JP3 bridged),
  `cart on`, and the firmware serving a jump in place of the `DK` header
  while it pulses, because the FIRQ entry is `$C000`.
- Loading `.BIN` and BASIC programs from a disk image without a DOS.
- Extended BASIC served at `$8000` from a Plus-W (`RP2350B_IDEAS.md` §15).

## 8. A14 (and A15) must reach the module on every build (hardware, required)

**Limitation today.** A Pico 2 build sees A0-A13 only. All 26 header GPIOs
are spent (`CLAUDE.md` "Pico 2 (module) only exposes 26 GPIO"), so A14 and
A15 are buffered by U13 but land on Plus-W pad-grid pins a Pico 2 does not
have. The firmware therefore cannot tell `$8000-$BFFF` from `$C000-$FEFF`.

**What it breaks (bench 2026-10-02, CoCo 3).**
- 32K Program Paks. Silpheed and Super Pitfall load but show corrupt
  graphics: a CoCo 3 maps a 32K pak at `$8000-$FEFF` and the two halves are
  told apart by A14. The firmware treats a 32K file as a `$FF40`-banked
  image, which these paks are not.
- Serving Extended BASIC at `$8000` (`RP2350B_IDEAS.md` section 15), for the
  same reason plus A15.
- Any later feature that decodes outside the `/CTS` and `/SCS` windows.

**Required.** A later hardware revision must give every build A14, and A15
with it if a pin can be found. This is a correction, not an option: a
cartridge that cannot run Tandy's own 32K paks is incomplete.

**Where the pin could come from (to decide at that revision).**
- Make the Plus-W (RP2350B, 48 GPIO) the only supported module. It already
  receives A14 on GP29 and A15 on GP30; the cost is the Pico 2 build.
- Free header pins on the Pico 2 build: `OE_BUS` could be derived in the
  module from `/CTS`, `/SCS` and E if those were routed in its place (three
  pins for one, so no gain by itself); header pin 34 is shared by audio and
  the JP5 `/CART` or `/NMI` drive; `HALT_GATE` is needed.
- Latch or multiplex in hardware: a small latch or a 2:1 mux could put A14
  and A15 on existing address pins during a part of the cycle the firmware
  does not use them, at the cost of read-path time. Needs a timing study
  against the fast-mode read path before it is believed.
- An RP2350B on the carrier in place of a module.

**Placement rule for the new pins.** A14 and A15 must land on the GPIOs
directly above A13, so A0-A15 are one contiguous run (and R/W next to
them). The PIO bus engine captures the address with one `IN PINS`; on the
Plus-W pad grid A14/A15 are GP29/GP30 with /CTS, /SCS, E and Q in between,
which a PIO capture cannot skip.

**Firmware side, once the pin exists.** `rom_load_mem` needs a linear 32K
mode (two 16K halves selected by A14, distinct from the `$FF40` banked
mode), and the manager needs to tell the two kinds of 32K image apart or be
told. The Plus-W cannot do this on today's board either: the PIO bus engine
(2026-10-02) takes A0-A13 with one `in pins`, and the pad-grid A14/A15 are
not next to A13 (rule above).

**Plus-W firmware-decoded addresses.** The CPU loop on a Plus-W also
answered `$FF60-$FF7F`, outside `/CTS` and `/SCS`, with JP2 at 2-3. That path was removed with the CPU loop (tag
`fw-1.4-cpu-loop`); the PIO engine holds `OE_FW` high and needs JP2 at 1-2.
It returns with the respin, alongside A14/A15.

