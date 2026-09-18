# PiCoCo — RP2350B feature ideas

Status: brainstorm, 2026-09-06. Nothing here is committed to the MVP.
The MVP remains DriveWire over the Becker port plus ROM emulation as
described in `hardware-design.md` and `firmware-architecture.md`.

The premise: swap the Pico 2 module (26 GPIO) for an RP2350B (48 GPIO).
The cart bus, not the chip, sets the ceiling on what can be done. This
doc records what the extra pins unlock, what the CoCo edge connector
makes impossible, and a suggested order of attack.

---

## 1. What the RP2350B changes

The current board spends its whole pin budget on D0..D7, A0..A13,
/R/W, E, OE_BUS and HALT_GATE. That is why A14/A15, Q, /SLENB, /CTS and
/SCS never reach the Pico and why U15 does the address decode in
hardware. With 48 GPIO everything gets routed and the Pico decodes
addresses itself.

Rough pin budget:

| Group | Pins |
|---|---|
| D0..D7 | 8 |
| A0..A15 | 16 |
| /R/W, E, Q, /SLENB, /CTS, /SCS | 6 |
| /HALT, /NMI, /CART drive (N-FETs) | 3 |
| U10 /OE (Pico-decoded) | 1 |
| Address/RW buffer DIR (DMA bus-master) | 1 |
| HSTX for DVI | 8 |
| Audio PWM | 1 |
| SD card, SPI | 4 |
| **Total** | **48** |

USB D+/D- are dedicated pins and not in the count. CoCo /RESET goes to
the RP2350's dedicated RUN pin, not a GPIO. A PSRAM chip select
(QMI CS1 on GP0/8/19/47) would push this over budget; drop SD to a
shared bus or skip PSRAM unless a feature below needs it.

Hardware notes:

- **HSTX lives only on GP12..GP19.** The contiguous address block on
  GP8..GP21 has to move if DVI is on the table.
- **QFN-80 is not hand-solderable.** Lazy path: a PGA2350 or similar
  RP2350B carrier that already breaks out all 48 GPIO, optionally with
  8 MB PSRAM. Avoids the bare-chip layout entirely.
- **U10 /OE moves to Pico control.** Rule: enable on
  `(cart-selected OR write cycle) AND E`. Writes are safe to pass on
  every cycle because DIR follows /R/W and the Pico side only listens.
  Non-cart *reads* must stay disabled, otherwise the floating Pico
  side gets driven onto the CoCo bus. This one change is what enables
  bus snooping, self-decoded I/O ranges ($FF6x/$FF7x), and /SLENB use.
- **Address and R/W buffers become bidirectional** (SN74LVC8T245 in
  place of U11/U12's LVC245, or LVC245 with DIR under Pico control) so
  the Pico can drive A0..A15 and R/W during DMA (§3). One GPIO for DIR.
  U10's DIR keeps following R/W, so it flips automatically when the
  Pico drives R/W. Lay out D0..D7 + A0..A15 as one contiguous 24-pin
  GPIO block so a single PIO `OUT PINS, 24` places a whole DMA cycle.
- **Two more N-FETs** next to Q2 for /NMI and /CART drive. Both
  already listed as v2 items in `hardware-design.md` §9.
- **SND (cart pin 35)** gets a PWM output stage: RC low-pass, divider
  to CoCo audio level, AC coupling cap.

---

## 2. Disk controller emulation — YES, highest value

The FD-502 is a WD1793 at $FF48..$FF4B plus a control latch at $FF40,
all under /SCS, so the existing decode already covers it.

Transfer mechanism: Disk BASIC sets the halt-enable bit ($FF40 bit 7)
and the controller holds /HALT low whenever DRQ is inactive, releasing
it as each byte becomes ready. INTRQ at command completion asserts
/NMI. Timing is generous: several microseconds between the CPU reading
$FF4B and the next instruction's HALT sample. A PIO catching the $FF4B
read plus a GPIO toggle covers it.

Needs: the /NMI FET. Already have /HALT via Q2.

**Lazier alternative: emulate the CoCo SDC register interface** rather
than a bare 1793. SDC-DOS and NitrOS-9 drivers already exist, its
native mode moves 512-byte blocks by polling with no halt, and it
includes a 1793-compatible mode anyway. Serve DSK images from SD.
Also gives SDC-DOS's ROM-bank features for free (see §6).

---

## 3. DMA and RAM expansion — DMA YES on CoCo 1/2, CoCo 3 with caveats

An earlier draft of this doc said bus mastering was impossible. Jim
Brain's PhantomRAM work proves otherwise:

- https://www.go4retro.com/2020/03/05/coco-dma-fighting-on-the-bus/
- https://www.go4retro.com/2020/03/12/coco-dma-invisible-ram/
- https://www.go4retro.com/2020/03/23/coco-dma-missing-without-a-trace/
- https://www.go4retro.com/2020/04/03/coco-dma-all-charged-up/
- https://github.com/go4retro/PhantomRAM

**Mechanism.** The cart pulls /HALT low. Once the 6809 finishes its
current instruction it tri-states A0..A15, D0..D7 and R/W. The cart
then drives all three, one bus cycle per E, and the SAM or GIME
performs a normal RAM cycle. BA/BS are not on the cart edge, so the
cart cannot see the CPU release the bus. Brain triggers DMA from a
16-bit store to a register pair, asserting /HALT on the first byte
access so the CPU is known to stop right after that instruction. For a
Pico-initiated transfer, wait the longest 6809 instruction plus
interrupt stacking (about 25 E cycles) after asserting /HALT.

| Model | Status |
|---|---|
| CoCo 1, CoCo 2, Dragon | Works unmodified. ~1 byte per E cycle. |
| CoCo 3, reads | Works unmodified. |
| CoCo 3, writes | 74LS245 between CPU and memory data bus is always enabled and fights the cart. Fix A: wire its pin 19 to CPU BA (one-wire mod, ~2 MB/s reported). Fix B: charge-storage trick, no mod. |

**CoCo 3 charge trick.** Drive the data during the first quarter cycle
with R/W = read and an address in $FFxx so the GIME stays off the bus.
The CPU-side pins and the '245 hold the value as capacitive charge
("charge up in 140nS or less and will hold its value for at least
840nS"). Release the data, flip to write, and the '245 amplifies the
stored charge onto the memory bus. Needs Q for the quarter-cycle
phasing, which is why Q is in the pin budget.

### 3.1 Confidence assessment for the charge trick (researched 2026-09-06)

| Question | Estimate |
|---|---|
| Physics is real; works on Brain's machines | 90% |
| Works on a random stock CoCo 3 with fixed timing | 60..70% |
| Works with per-machine auto-calibration + write-verify | 85%+ |
| A failure goes undetected | ~0% (reads always work; verify every write) |

Supporting evidence:

- Brain and Darren Atkinson (CoCo SDC designer) tested both MC6809E and
  HD63C09E with stock memory, DRAM-based and SRAM-based 512K
  expansions: "Initial results, though, look promising." DAT boards
  (Boomerang E2, Triad+) untested, but they sit in the DRAM sockets on
  the memory side of the '245, downstream of the storage node.
- Implemented in PhantomRAM HDL behind a `mode_cc3` flag (commit
  2020-04-07, "Added in code for unmodified CoCo3 operation").
- The transceiver is IC3 (74LS245, enable grounded) per the Tandy
  service manual. No source shows its wiring differing between board
  runs. GIME 1986 vs 1987 is a chip-level video/IRQ fix, not a data
  path change. GIME-X adds level buffers for its own FPGA only.
- A 74HCT245 swap by a repairer improves the trick (CMOS input, no
  input current).

Weaknesses:

- Single source. No public replication in six years; repo dormant
  since April 2020; Brain's shipped products (CocoMEM, CocoMEM Jr.)
  are GIME/CPU socket interposers, not cart-port DMA.
- Stated hold "at least 840nS" equals the slow-mode requirement, so
  slow-mode margin is unquantified. Fast mode needs only 420 ns hold
  with a 140 ns charge window; the '245 output charges the node in ns.
- Physics of the variance: after the R/W flip the CPU-side node is
  loaded by an LS input, which sources up to 0.2 mA when held low.
  Into ~30 pF that would lift a stored low through threshold in
  ~100 ns, far short of the measured 840 ns, so Brain's parts draw well
  under spec max. Highs are safe (µA leakage only); lows depend on the
  IC3 lot's actual input current, which spreads 5..10× typical to max.
  Temperature is a second-order effect. This one TTL parameter is the
  whole fleet risk.

Mitigation the Pico enables that the FPGA design did not:

- Tune the release point per machine in firmware.
- Self-test at boot: DMA-write a pattern to a scratch area, DMA-read it
  back (reads always work on CoCo 3), enable write DMA only on pass.
  Fall back to read-only DMA plus the windowed RAM cart otherwise.
- Optional write-verify per burst at 2× cost.
- Document the pin-19-to-BA mod (Brain's "DMEnabler") for the failing
  tail. No user-facing feature should depend on CoCo 3 write DMA.

Sources: go4retro.com CoCo DMA series (Feb..Apr 2020),
github.com/go4retro/PhantomRAM (hdl/PhantomRAM.v), Tandy CoCo 3
Service Manual (archive.org), thezippsterzone.com GIME-X page.

**Hardware.** /HALT drive already exists (Q2). Address and R/W
buffers become bidirectional with a Pico-controlled DIR (§1).

**Firmware.** One PIO program: wait E low, `OUT PINS, 24` for address
plus data, wait E high, wait E low. Fed by DMA from SRAM or PSRAM.
Keep bursts short (sub-millisecond) so BASIC's 60 Hz timer and
interrupt latency stay sane.

**What it buys:**

- **PhantomRAM-style invisible RAM.** Byte-granular expansion with no
  banking, backed by SRAM or 8 MB PSRAM. The CoCo copies between its
  RAM and the cart's via registers. Brain used $FF67..$FF69.
- **Fast loading.** A 32 KB program lands in CoCo RAM in ~37 ms at
  0.89 MHz. Sector reads for disk emulation (§2) or DriveWire land
  straight in the DOS buffer, no per-byte halt loop. Needs a patched
  DOS or a NitrOS-9 driver.
- **Video shadow resync.** DMA-read all 64 KB at startup or whenever
  the snoop (§4) might have missed something. Not a substitute for
  snooping: reading a 32 KB CoCo 3 screen costs ~18 ms of halt per
  frame, which is the whole CPU at 60 fps.
- **Real in-circuit debugger.** Dump and poke any address from the
  host, plant SWI breakpoints, save/restore machine snapshots
  (RAM via DMA, CPU registers via an NMI handler in cart ROM).
- **Coprocessor.** Pico reads arguments from CoCo RAM and writes
  results back, no window protocol needed.
- **CoCo 3 full 512 KB access.** DMA-write the MMU registers to map any
  physical page into a window, read it, restore. Write side needs
  Fix A or B above.

**Paged RAM cart still worth having** as the no-DMA fallback and for
NitrOS-9 RAM disk use: 520 KB on-chip SRAM makes a 256..384 KB paged
cart, and /HALT hides the page-copy time.

**Open question — verify at the `fw-0.4-bus-capture` milestone:**
whether /CTS asserts on write cycles. My recollection is the SAM routes
all writes to RAM in map type 0, making /CTS read-only. If so, the
paged cart's write side goes through the 32-byte /SCS window as an
address-register plus auto-increment data port, while reads stay
memory-mapped in the 16 KB /CTS window. On the CoCo 3, /SLENB lets the
cart claim any address on both reads and writes. Also verify that
/SLENB actually suppresses the internal RAM write on the CoCo 3.

---

## 4. Graphics card — CoCo 1/2 via bus snooping; CoCo 3 not planned

**Decision (2026-09-08):** build the VDG snoop for CoCo 1/2 only. The
HSTX/HDMI hardware stays on the board for two reasons: the CoCo 1/2
snoop below, and the WordPak-RS 80-column renderer (§12), which is the
only HDMI feature that applies to a CoCo 3. Full GIME video emulation
is dropped, not deferred.

**Why CoCo 1/2 and not 3.** A stock CoCo 1/2 has RF out only; composite
needs a soldering mod before any cheap dongle helps. A cart that gives
HDMI with no mod is the only no-solder path and nothing else offers
it. A CoCo 3 already has composite and analog RGB, so a composite
dongle or RGB2HDMI is cheaper than firmware that must mirror the MMU,
16 palette registers, 40/80-column text, and the raster tricks that
CoCo 3 demos and games use everywhere. Low value, high cost.

**Why it is worth having on a CoCo 1/2.**

- **A display at all.** Modern TVs are dropping analog tuners, and RF
  through the ones that remain is blurry enough that 32-column text is
  hard to read. HDMI from the cart gives a pixel-exact picture with no
  soldering inside a 40-year-old machine, and it comes out when the cart
  does.
- **Sound on the same cable.** A stock CoCo 1/2 has no audio output
  except the TV. Snooping the $FF20 DAC (and mixing the cart's own
  synth, §5) puts picture and sound on one HDMI cable.
- **Chosen artifact phase.** Real hardware picks the artifact phase at
  random on power-up, and people power-cycle until the colours come out
  right. Here it is a setting (§4.1).
- **Real lowercase.** The renderer can apply the Lowerkit/Dragon 200e
  rule and show true lowercase for Color BASIC's inverse-video
  convention, with no VDG socket work (§10).
- **Capture and remote use.** A pixel-exact frame already in SRAM makes
  screenshots over USB, video capture, and a browser-based remote CoCo
  (§9, keyboard injection over WiFi) nearly free. Today that needs a
  capture card on RF or composite.
- **Dual display with WordPak-RS.** The 80-column console renders to
  HDMI while the VDG picture stays on the TV, or both go to HDMI. On a
  CoCo 1/2 that is an 80-column NitrOS-9 console with no monitor at all.

**No video signal reaches the cart edge.** The VDG fetches from RAM
through the SAM on cycles the cart never sees. "Intercepting video" is
not possible from the slot.

**What is possible: shadow the RAM and render it yourself.** Every CPU
write appears on the cart bus with full address and data, and so do the
writes that set the SAM video mode and display offset ($FFC0..$FFDF)
and the PIA1 VDG mode bits ($FF22). Verified against the factory
schematics (2026-09-08): on the CoCo 1 (TRM 26-3193 and Dwg 8000073)
and both CoCo 2 boards (Dwg 8000191 Rev D, and the Korean SN74LS785
board) D0..D7 and R/W are bare wires from the 6809 to the cartridge
connector. No buffer, no gating by /CTS or /SCS. A margin note on the
CoCo 1 factory drawing reads "write to ram is directly off data
lines". The CoCo 3 adds a 74LS245 with enable grounded, so the same
holds there. Sources:
https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/Color%20Computer%201%20Schematic%20(Tandy).pdf
https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/Color%20Computer%202%20Schematic%20(Tandy).pdf
https://colorcomputerarchive.com/repo/Documents/Manuals/Hardware/Color%20Computer%202%20Schematic%20(Rev.%20A)%20(Tandy).pdf
Our U10 is therefore the only thing between the CoCo bus and the Pico,
and the §1 enable rule is sufficient as written. The breadboard bus
watcher confirms it for free during bring-up. The Pico shadows the 64 KB RAM,
mirrors the mode registers, and renders the way the real VDG does,
either whole-frame into an RGB565 buffer or scanline-by-scanline into a
line buffer ahead of HSTX. Either fits SRAM. HSTX encodes TMDS in
hardware, so DVI scanout is DMA.

Hardware it needs, all already in §1: HSTX on GP12..GP19 (address block
moves), A14/A15 routed, and U10 /OE under Pico control with the
cart-selected-OR-write rule so the data bus is visible on every write.

Firmware cost: capture every bus cycle in PIO (address, data, R/W;
one 32-bit word per E), DMA into a ring buffer, apply writes from the
core that owns the response table, never in the bus-cycle path. Capture
all cycles, not just writes, from day one: the ring index is then a
cycle-exact timestamp, which stage 2 (§4.3, §4.4) needs and which costs
nothing to record now. A frame is 14934 words, about 60 KB; a
double-buffered ring is 120 KB and walking it is about 6% of one core.
About 80 CPU cycles per bus cycle at 1.79 MHz, fine in tight code.
Shares the capture path with `bus_watcher`. Rendering a PMODE 4 frame
from a 6 KB shadow is under 1 ms at 60 Hz.

Bonus: writes to the $FF20 DAC are on the bus too, so the HDMI stream
can carry CoCo audio, and the cart's own audio (§5) can be mixed in.

### 4.1 Artifact colors

PMODE 4 games depend on NTSC artifacting; a literal render shows black
and white stripes. Emulate it in the shadow-to-pixel step as one table
lookup per PMODE 4 byte. Two candidate rules, same cost:

- **Three-tap rule** (the GLSL shader in the author's Dungeons of
  Daggorath port, `src/artifact_shader.h`): a white pixel with white on
  both sides stays white; otherwise it takes orange or cyan by x
  parity, blended 35% toward white if one neighbour is white; black
  stays black. Output depends on the byte plus one bit from each
  neighbouring byte: 1024-entry table, 8 output pixels per entry.
- **Pair rule** (XRoar/MAME simple mode): each pixel pair is one
  128-wide artifact pixel; 00 black, 11 white, 01 and 10 the two
  colours. 256-entry table, no neighbour bits. Renders solid colour
  fields correctly, which the three-tap rule shows as stripes.

Both are bound by writing pixels out, not by choosing colours, so pick
by appearance. Start with the three-tap rule because it is known to
look right on Daggorath, and swap the table builder later without
touching the render loop. Table size is 16 KB (palette indices) or
32 KB (RGB565) for both phases. The two hues, the blend weight, and the
phase flip are config values: real hardware picks the phase at random
on power-up and people argue about the hues. Artifacting applies only
in PMODE 4 and only when enabled.

### 4.2 Frame sync

No vsync reaches the cart edge, and the Pico's DVI frame rate is not
locked to the VDG, so a naive render tears at the beat frequency.
Infer vertical blank from bus activity instead: the VDG's FS line
drives PIA0 CB1, and both Color BASIC's IRQ handler (which reads $FF02
to clear it) and polling games (which read $FF03) touch PIA0 once per
field. Snapshot the shadow at that moment into the render buffer.
Addresses are visible on read cycles, so this needs no data-bus
access. Software that syncs some other way can still tear; accept it.

### 4.3 Mid-frame register changes

Split screens and per-line colour-set changes happen by writing $FF22
or the SAM bits partway down a frame, timed from the HSYNC interrupt or
cycle-counted from FS. A renderer that samples registers once per frame
gets these wrong. This is not an information problem, because the write
and its cycle are both on the bus. It is a phase problem, and the
CoCo's clock tree makes it tractable:

- **The bus is a perfect raster clock.** E and the VDG pixel clock come
  from the same 14.318 MHz crystal. A scanline is exactly 57 E cycles
  and a frame exactly 14934. A count of bus cycles never drifts from
  the picture. Only the phase (which cycle is the top of the frame) is
  unknown.
- **The phase comes from the software's own sync.** Any program doing
  raster tricks must sync to FS or HS through PIA0, the only sync source
  a CoCo 1/2 has, and those accesses are visible: the IRQ vector fetch
  at $FFF8 (stacking is fixed, the instruction remainder adds 0..20
  cycles of jitter, so take the minimum over many frames), the HS
  interrupt every 57 cycles, or the exit of a `LDA $FF03 / BPL` polling
  loop. Color BASIC's FS interrupt runs at 60 Hz from power-on, so the
  lock is tight before any game starts, and it holds through sync-once
  demos because the count never drifts.
- **Render by replaying the ring in beam order.** Walk 262 lines, apply
  every write whose cycle falls before the line ends, draw the line with
  the registers as they stood. Mode, colour-set and display-offset
  changes land on the right line; RAM writes that race the beam land
  above or below it correctly. Horizontal placement is one E cycle,
  8 PMODE 4 pixels, about what real hardware glitches over anyway.

Residual limits: a Pico reboot mid-session loses the count, and
sync-once software is off by up to a third of a line until it syncs
again (software that touches PIA0 every frame re-locks in a second);
address-dependent fast mode (POKE 65495) shortens ROM/IO cycles so the
count slips a few cycles within a frame, bounded by per-frame re-lock;
software with no PIA0 sync cannot be located, but cannot do raster
tricks either. Output is always one frame behind.

**How much software this affects (searched 2026-09-08): almost none.**
One commercial CoCo 1/2 title is documented doing it: Dragonfire
(Tandy, 1984) toggles CSS at $FF22 several times per scanline for up to
nine colour sets per line. Two independent sources: the Stupid VDG
Tricks blog (http://vdgtricks.blogspot.com/2012/07/palette-expansion.html)
and the DracoDS emulator README, which admits the game is "not
color-accurate" because "mid-scanline CSS trickery is not supported"
(https://github.com/wavemotion-dave/DracoDS). The SAM-graphics /
VDG-text "inconsistent mode" trick is documented on the Dragon Archive
wiki but no shipped title using it was found. Sock Master's demos are
all CoCo 3. Dungeons of Daggorath draws its text as vectors in one
PMODE 4 bitmap (the glyphs carry the same artifact fringing as the
graphics), so it is a single mode per frame. Forum archives were mostly
unreachable, so a title known only from forum lore could have been
missed. Note that Dragonfire's trick is several changes per line, so a
per-scanline register model would still get it wrong; only the
cycle-timestamped replay above handles it.

### 4.4 Plan: two stages

**Stage 1, the version that ships.** Capture every cycle into the ring
(§4). Render whole frames from the shadow, snapshotting at inferred
vblank (§4.2), with per-frame register state. Artifact table (§4.1).
Restart recovery (§4.5). This covers BASIC, essentially every
commercial game, Daggorath, and the WordPak-RS console. Output is an
emulated VDG view of RAM, not the VDG's signal, and the doc should say
so.

**Stage 2, only if a real title demands it.** Beam-order replay from
the same ring (§4.3). The capture path and board do not change, so this
is renderer-only work. Dragonfire is the acceptance test. If nobody asks
for Dragonfire, stage 2 never happens and nothing was wasted.

**Not planned:** GIME video emulation (RGB out plus a converter wins on
every axis), PSRAM for the video path (neither stage needs it), and
genlocking the DVI clock to the CoCo (one duplicated frame a minute is
imperceptible). Priority stays behind DriveWire, disk emulation and
sound as in §7; the HDMI hardware earns its place through WordPak-RS
regardless.

### 4.5 Surviving a Pico restart

A CoCo reset also resets the Pico (RUN pin) and Color BASIC rewrites
every SAM/PIA register on the way up, so that case is free. The case
that matters is the Pico restarting while the CoCo keeps running: a
firmware update from the console, a deliberate `reboot`, or a crash
that trips the watchdog. Two existing mechanisms cover it with almost
no new code:

1. **The CoCo freezes for the duration.** HALT_GATE defaults to held at
   boot (R7 pull-up on the Q2 gate), so the moment the Pico resets and
   GP27 floats, /HALT goes low and the 6809 stops at the end of its
   current instruction. `halt_release()` runs only after `bus_watcher`
   is up. No write can be missed because no write happens. The SAM
   keeps refreshing DRAM and the VDG keeps displaying the frozen
   screen.
2. **The shadow survives in SRAM.** Put the 64 KB shadow and the
   mode-register mirror in `.uninitialized_data` with a magic word and
   a CRC. RP2350 SRAM is retained across watchdog and software resets
   and cleared only by power-on. At boot, read the chip reset reason:
   watchdog or software reset with a valid magic and CRC means keep
   the shadow; RUN-pin or power-on reset means zero it and let the CoCo
   ROM repopulate.

Together these make planned and unplanned restarts recover to an exact
picture with zero missed cycles. Firmware updates should therefore go
through the console updater and a watchdog reboot, not BOOTSEL, since
the bootrom's USB mode makes no promise about SRAM. Reading CoCo RAM
back over DMA (§3) is not needed for resync on CoCo 1/2.

---

## 5. Speech and sound — YES, cheap

Cart pin 35 (SND) is an analog audio input the CoCo mixes to its TV
output when the cart source is selected (`AUDIO ON`). Hardware is a
PWM or sigma-delta output through RC filter, divider and coupling cap.

The Pico captures the register writes and does the synthesis:

- **AY-3-8913 / YM2149** — trivial to emulate (CoCo PSG compatible).
- **Tandy Speech/Sound Pak** — SP0256 cores exist; also had a TMS7000
  front end whose protocol has to be reproduced. Registers at
  $FF7D/$FF7E.
- **Orchestra-90** — two 8-bit DACs at $FF7A/$FF7B. Mono into SND, or
  stereo via a jack on the cart.

Decode gotcha: $FF7x is outside both /CTS and /SCS, so the cart must
decode those addresses itself. That is exactly what Pico-controlled
/OE (§1) provides. Same change the video snoop needs.

---

## 6. Other things the same hardware buys

- **Multi-Pak emulation and a ROM library.** Emulate the $FF7F slot
  register and present several virtual carts at once. Load any .ccc or
  .rom from SD, including 32 KB bank-switched titles that write $FF40.
- **RS-232 Pak, and with it a WiFi modem.** A 6551 ACIA at
  $FF68..$FF6B is a handful of registers. The RP2350B has no radio;
  pair it with an ESP32-C3 over UART or a W5500 over SPI. Also lets
  DriveWire run over TCP to a networked server instead of USB.
- **Real-time clock.** RP2350 has an always-on timer. Emulate the
  DS1315 (SmartWatch) or Disto RTC register interface; NitrOS-9
  already has drivers. Add a backup cell or sync time from DriveWire.
- **Bus analyzer and in-circuit debugger.** The snoop path streams a
  full 6809 bus trace over USB. /HALT plus /NMI give address
  breakpoints. Nothing like it exists for the CoCo.
- **USB mass storage host** for thumb-drive DSK images via TinyUSB
  host. Keyboard or gamepad passthrough is *not* possible: those live
  on the PIAs, not the cart bus.
- **Coprocessor tricks.** The CoCo writes a command into the RAM
  window, the Pico computes and writes results back. Niche, but free
  once the RAM cart exists.

Not possible from the slot, for the record: CPU acceleration,
keyboard/joystick injection, capturing the real video signal.

---

## 7. Suggested order

1. Build the main carrier so it takes either a Pico 2 or a Waveshare
   RP2350B-Plus-W (§13). With the Plus-W fitted, every cart signal
   reaches the chip and a solder jumper hands U10 /OE to firmware with
   the cart-selected-OR-write rule. One board, no second spin.
2. Add the /NMI and /CART FETs next to Q2.
3. Wire SND with a PWM output stage; put SD on the board.
4. Firmware, in order of value: DriveWire as planned → SDC-style disk
   emulation → sound → DMA on CoCo 1/2 (RAM expansion, fast load,
   debugger) → RAM cart fallback → CoCo 1/2 HDMI snoop → WordPak-RS
   HDMI renderer → CoCo 3 DMA writes.

Early verification items (bus-capture milestone): /CTS on write
cycles; /SLENB behaviour on CoCo 3 writes. Both decide how the RAM
cart's write side works.

---

## 8. What coexists

Nearly everything above runs in one firmware at once, because the
features live at different addresses and share a single bus-capture
path.

**Coexists freely:**

- **DriveWire Becker + disk emulation + RAM cart write port** all sit
  under /SCS at different addresses ($FF41/42, $FF40 and $FF48..4B,
  spare $FF5x). A real CoCo with a floppy controller and a Becker port
  does exactly this; NitrOS-9 loads both drivers.
- **Sound, RS-232, RTC, MPI register** are self-decoded at $FF6x/$FF7x,
  each at its own address. They stack with everything.
- **Video snoop + debugger** share the same write-cycle capture.
- **All of the above together** fit CPU and RAM on a CoCo 1/2:
  ROM + 64 KB shadow + audio/line buffers + ~256 KB RAM cart.

**The rule that makes it work:** every value the CoCo can read is
served by PIO + DMA from a response table in SRAM indexed by address.
core1 updates the table asynchronously. SD latency, synthesis and DVI
output never sit in the bus-cycle path. Register-style devices (1793
status, ACIA, PSG read-back) precompute their readable state into the
table instead of responding live.

**Real conflicts:**

| Conflict | Why | Resolution |
|---|---|---|
| USB host vs USB device | One port, one role at a time | Use SD or WiFi/TCP for the DriveWire server if thumb drives matter |
| /CTS ROM window | One 16 KB ROM visible at a time | MPI emulation to slot-switch, or one DOS ROM supporting both DW and SDC |
| Pins for WiFi UART or PSRAM CS | §1 budget is exactly 48 | Free two: /RESET goes to the dedicated RUN pin, not a GPIO; Q is optional |
| /NMI ownership | Disk INTRQ vs debugger breakpoints | Time-multiplexed, one owner at a time |
| /HALT ownership | Disk per-byte halt, RAM page swap, DW flow control, DMA bursts | Same: firmware arbitrates, one owner at a time; the CPU is halted during each anyway |
| DMA vs live video | DMA-reading a screen halts the CPU for most of a frame | Video stays snoop-based; restarts recover from the retained SRAM shadow (§4.5), no DMA needed |
| DMA writes on CoCo 3 | Needs the pin-19 mod or the charge trick | Reads, and everything else, work unmodified; gate write-DMA features behind a per-machine setting |

DMA and the bus snoop are complementary: the snoop path already
captures every cycle, and the DMA path reuses the same PIO pins in the
opposite direction. Neither adds pins the other doesn't need beyond the
one DIR line.

---

## 9. Creative uses of DMA + snoop + /NMI

Brainstorm only. Everything here rests on the DMA path (§3), the
write snoop (§4), and the /NMI FET.

**Halt the 6809 forever and become the CPU.** Hold /HALT permanently
and run a 6809 emulator on the RP2350, driving the real SAM, VDG and
PIAs through DMA. Once the Pico has DMA-read all 64 KB its shadow is
authoritative (nothing else writes RAM), so reads are free and only
video-RAM writes and I/O touch the bus at one byte per E. Estimated
10..40× a stock CoCo 1/2 with the real hardware still doing output.
/IRQ and /FIRQ are not on the cart edge: poll the PIA flag registers
over DMA at ~1 kHz. CoCo 3 needs the write path and a GIME-aware
emulator; CoCo 1/2 first.

**Freezer cart: save states and rewind.** Pulse /NMI; the 6809 stacks
all registers. A cart-ROM handler parks SP somewhere known and reads
the PIAs, then the Pico DMA-reads 64 KB. SAM/VDG state is write-only
but the snoop mirrors it. Restore: DMA-write RAM, ROM code rewrites
PIAs and SAM, RTI. Because the snoop logs every write against the
shadow, a delta ring gives rewind.

**Loading and control**

- **Instant tape/disk loads.** Parse CAS/BIN on the Pico, DMA blocks to
  their load addresses, jump via the RAM NMI vector. No DOS.
- **Remote execute primitive.** DMA a routine into RAM, point Color
  BASIC's RAM NMI vector at it, pulse /NMI.
- **Keyboard injection.** Color BASIC's RAM hooks include console
  input; install a hook and feed keystrokes. Paste listings, autostart
  scripts, USB keyboard passthrough, browser-based remote CoCo when
  paired with the HDMI snoop over WiFi.
- **Extension BASIC.** Cart ROM adding commands executed by the Pico
  over DMA: fast load, PLAY through the synth, accelerated DRAW/blits.

**Video and audio**

- **Pico as blitter.** Compose on the Pico, DMA only dirty bytes into
  video RAM. The VDG keeps fetching during halt, so no glitches.
- **Video playback.** PMODE 4 frame = 6 KB ≈ 7 ms of bus at 0.89 MHz;
  30 fps costs ~20% of the bus.
- **PCM through the CoCo DAC.** $FF20 is a bus address; DMA samples in.
  Cheap when the CPU is already halted; otherwise use SND.
- **Virtual printer.** Bit-banger TX is a bit in $FF20; timestamp the
  snooped writes and decode.
- **Gameplay capture.** Snoop renderer + DAC writes → record to SD or
  stream.

**Developers and preservation**

- **Ground-truth bus traces** for emulator authors and copy-protection
  timing.
- **Dead-CoCo diagnostics.** With the CPU halted, DMA tests every RAM
  chip, exercises the PIAs, dumps the internal BASIC ROMs.
- **Memory-freeze cheats.** Rewrite an address whenever the snoop sees
  it change.
- **Shared memory over the network.** Two carts mirror a RAM region
  over WiFi for two-player games between real CoCos.

Dragon 32/64 use the same signals on a rotated connector and Brain's
DMA works there, so most of this ports with a different edge footprint.

---

## 10. Jim Brain's other CoCo work, mapped to PiCoCo

Surveyed go4retro.com on 2026-09-06. None of it is a cart-port
technique; three threads feed features already listed.

**MC6847 lowercase / external font series (2024-10-29 to 2024-11-17).**
Wires an external character ROM to the VDG via its socket, using the
chip's row counter plus the display byte to address glyphs. Not
reachable from the cart edge. But the goal is free on the HDMI snoop
renderer (§4): Lowerkit and Dragon 200e boards select the lowercase
set when the inverse bit is set, which is Color BASIC's lowercase
convention, so the renderer applies the same rule and shows real
lowercase with no CoCo change. Brain published clean binaries of 32
font sets (original 6847, Dragon 200e Spanish, Greek, Katakana, APL,
ZX Spectrum, TI-99/4A, CGA) for a font picker.
- https://www.go4retro.com/2024/10/29/how-low-can-you-go-2/
- https://www.go4retro.com/2024/10/31/expanding-the-mc6847-deciphering-fonts/
- https://www.go4retro.com/2024/11/08/expanding-the-mc6847-deciphering-the-interface/
- https://www.go4retro.com/2024/11/16/expanding-the-mc6847-deciphering-fonts-part-2/
- https://www.go4retro.com/2024/11/17/expanding-the-mc6847-deciphering-the-interface-part-2/
Lesson from the interface post: 74HCT in place of 74LS changed input
pull behaviour and clipped the top scanline until bias resistors were
added. Any vintage LS circuit we reproduce with LVC parts needs its
floating inputs biased deliberately.

**Sound projects.** CocoSOUND (dual AY-3-8910 + optional SN76489,
2017, unreleased), Philharmonic-12 (12-voice AY kit, in store), CoCo
SDC Extender (YMF262 OPL3 + 512 KB SRAM + flash). All register-mapped
and emulable on the Pico; PicoGUS shows OPL emulation on RP2040.
Prioritise register maps with software behind them (AY via CoCo PSG).
Adds: **a stacked sound cart** emulating AY, SN76489, OPL3,
Orchestra-90 and the Speech/Sound Pak at their own addresses at once,
stereo out on a jack.
- https://www.go4retro.com/projects/cocosound/
- https://store.go4retro.com/tandy/philharmonic-12/
- https://www.go4retro.com/projects/coco-sdc-extender/

**Memory projects.** CocoMEM (GIME socket, up to 64 MB, dual MMU),
CocoMEM Jr. (6809 socket for CoCo 1/2/Dragon), and the 2025 GIME DRAM
wiring post. Confirms real CoCo 3 memory expansion happens at the
socket, not the cart, and that the DMA charge trick was research, not
his product path. Nothing reachable from the slot. The SDC Extender's
SRAM + flash under a Program Pak shell is the RAM cart + ROM library
already in §3/§6.
- https://www.go4retro.com/projects/cocomem/
- https://www.go4retro.com/projects/cocomem-jr/
- https://www.go4retro.com/2025/09/30/more-memory-gimme/

Not opened: "Coming Soon: SuperOS/9 MMU Kit" (2013, 6809-socket MMU
for OS-9 on CoCo 1/2; socket-based, not cart).

---

## 11. MIDI and the MT-32

Fits the stacked sound cart (§10). The MT-32 needs only a MIDI DIN;
an MPU-401 is not required.

**Hardware.** MIDI = 31250 baud UART. OUT: one GPIO, a gate, two
resistors, DIN-5 or TRS jack. IN: second GPIO behind an optocoupler,
only for recording from a keyboard. Pin budget (§1) is at 48, so OUT
costs the Q line, or move the timing-tolerant /NMI and /CART drives to
a small I2C expander. USB MIDI is free on top: drive Munt on a PC for
users without an MT-32.

**CoCo-side interfaces, all coexisting:**

- **Rulaford MIDI Pak emulation**: 6850 ACIA at $FF6E (ctrl/status)
  and $FF6F (data), per MAME `coco_midi.cpp` and the Rulaford manual.
  Lyra, UltiMusE III and Coco MIDI Pro drive it; Go4Retro's MIDI
  Maestro is register-compatible. Dragon MIDI is the same ACIA at
  $FF74/$FF75. This is the preferred MIDI path.
- **Raw MIDI port**: data register with a deep FIFO, for new software.
- **Chip-to-MIDI translation**: watch writes to the emulated SN76489 /
  AY and emit note on/off, velocity from attenuation. Same idea as
  ScummVM's MIDI output for AGI.

**Sierra AGI on the CoCo 3.** No AGI game had MT-32 music natively on
any platform; MT-32 support arrived with SCI (1988) and no SCI title
came to the CoCo. AGI SOUND resources are SN76496 register streams
(3 tone + noise) written for the PCjr / Tandy 1000. So:

1. Check whether the CoCo 3 disks kept the SOUND resources (`SNDDIR`,
   `VOL.*`) with an AGI resource tool.
2. If stripped, transplant from the PC versions; the format is
   identical across AGI ports, disk space permitting.
3. Patch the interpreter's sound player to write each tick's channel
   updates to the cart's SN76489 port instead of the DAC. Small 6809
   patch; the interpreter already decodes the stream.
4. Output selectable: DAC, cart audio (exact Tandy arrangement),
   MT-32 via translation (new arrangement of the same notes, with a
   SysEx patch set pushed at boot so instruments suit AGI), or all.

**Stretch.** Small SoundFont GM synth on the Pico at low polyphony:
feasible. MT-32 emulation (Munt) on a Cortex-M33: not feasible.

---

## 12. Emulation target catalog

Goal: let people run as much CoCo software as possible without owning
the original peripherals. Catalogued 2026-09-06 from MAME
`src/devices/bus/coco/*.cpp`, `sdc.org/~goosey/coco/cocoio.txt`,
Lomont's CoCo hardware notes, cocopedia and vendor pages. **Spot-check
every address against the raw MAME source before hard-coding**;
summaries can mangle hex.

### 12.1 Address map ($FF40..$FF9F) and conflicts

| Address | Device | Notes |
|---|---|---|
| $FF40 | FDC control latch; GMC bank reg | **conflict**, profile-switched |
| $FF41 | Becker status (read); GMC SN76489 data (write) | read vs write, can coexist |
| $FF42 | Becker data | |
| $FF48..4B | WD1793/1773 (FD-50x, Disto, SDC 1793 mode) | SDC native mode shares these |
| $FF50..58 | Glenside IDE (default; alt $FF70..78) | Cloud-9 SuperIDE likely same, unconfirmed |
| $FF50/51 | Disto RTC (unconfirmed) | |
| $FF5A..5F | CoCo PSG (bank, ctrl, YM reg/data) | base inferred from SCS offsets |
| $FF60..63 | Symphony 12 (PIA → 4× AY-3-8910) | |
| $FF60..67 | CoCo Max A/D module | **conflict** with Sym12, CoCoFLASH |
| $FF64..67 | CoCoFLASH config | |
| $FF60..6A | PhantomRAM DMA regs (§3) | pick our own DMA regs elsewhere |
| $FF68..6B | Deluxe RS-232 Pak (6551) | |
| $FF6C..6F | Direct Connect Modem Pak (6551) | skip; MIDI wins $FF6E/6F |
| $FF6E/6F | Rulaford MIDI Pak (6850) | |
| $FF70..73 | Stereo Composer (PIA, 2× DAC) | |
| $FF70..74 | Votrax "Sweet Talker" speech (SC-02) | **conflict** with Stereo Composer |
| $FF74..77 | Disto Super Controller II no-halt cache | |
| $FF74/75 | Dragon MIDI (6850) | Dragon profile only |
| $FF76 | WordPak-RS (R6545, CoCo 3 safe) | |
| $FF78..7B | WordPak 2+ (V9958) | **conflict** with Orch-90 |
| $FF7A/7B | Orchestra-90 L/R DACs | |
| $FF7D/7E | Tandy Speech/Sound Cartridge | |
| $FF7F | Multi-Pak slot register | ghosts at $FF9F on real MPI |
| $FF82 | Real Talker (Votrax SC-01, unverified) | |
| $FF98 | WordPak I/II (6845) | CoCo 1/2 only; $FF9x is GIME on CoCo 3 |
| $C000..$FEFF | ROM paks; 32K bank paks write $FF40..5F offset reg | |

Resolution: **profiles = virtual MPI slots.** A profile is a set of
enabled devices. The default profile holds every device that does not
conflict; ROM paks carry a manifest naming their profile (GMC games,
CoCo Max, WordPak 2+). The $FF7F register can also switch profiles for
MPI-aware software.

### 12.2 Tiers

**Tier 1 — real software base, cheap to emulate, all coexist:**

| Device | Why | Output |
|---|---|---|
| FD-50x WD1793 + CoCo SDC (§2) | RS-DOS, HDB-DOS, SDC-DOS, NitrOS-9 | SD/USB images |
| Becker / DriveWire | MVP | USB/TCP |
| Rulaford MIDI Pak (§11) | Lyra, UltiMusE III, Coco MIDI Pro | MIDI DIN + USB MIDI |
| Orchestra-90 | Orch-90CC, Soviet Bloc, Gems, OS-9 Play, NitrOS-9 patches | stereo jack |
| Speech/Sound Cartridge | Card King and many "SSC" titles; OS-9 L2 driver. Emulate TMS7040 protocol + SP0256 + AY-3-8913 | SND pin |
| CoCo PSG | YM2149 trackers/demos, gamepad ports | SND pin |
| Games Master Cartridge | Fahrfall, Dunjunz GME, Blockdown, Treasure Island Defence (SN76489 + banked ROM) | SND pin |
| Deluxe RS-232 Pak | terminals, BBS software, OS-9 SCF; with ESP32 = WiFi modem | UART/WiFi |
| Multi-Pak $FF7F | MPI-aware software, profile switching | — |
| Glenside IDE | HDB-DOS, NitrOS-9 hard-disk images | SD |
| RTC (Disto/Cloud-9 map, verify) | NitrOS-9 clock modules | AON timer |
| WordPak-RS | NitrOS-9 + DECB 80-column drivers, O-PAK, DynaStar | **HDMI** (6845 text renderer) |
| 32K bank-switched paks, CoCoFLASH scheme | RoboCop, Predator, big homebrew ROMs | ROM library |

**Tier 2 — niche but well-defined:**

- Symphony 12 (4× AY), Stereo Composer (2× DAC): their own bundled
  editors only.
- WordPak I/II at $FF98 (CoCo 1/2), WordPak 2+ (V9958: heavier, MSX
  VDP emulators exist; brings MSX-class graphics to HDMI).
- Disto Super Controller II no-halt cache ($FF74..77).
- CoCo Max A/D module ($FF60..67): USB mouse → CoCo Max on CoCo 1/2.
- X-SID (MOS SID cart, Nigel Barnes): SID emulation feasible, tiny
  software base.
- Votrax speech carts (Real Talker, Sweet Talker): SC-01/SC-02
  emulation exists in MAME.
- Dragon set: DragonDOS (WD2797), Delta DOS, Dragon MIDI, JCB Sound
  Extension (AY at $FEFE/FF) and Speech modules, MSX2 video cart,
  sprites board. Needs a Dragon profile and confirmation that the
  physical edge is interchangeable.

**Tier 3 — skip or later:** Direct Connect Modem Pak (300 baud modem,
RS-232 Pak covers the use), DS-69B Digisector and Rascan (need a video
ADC; could front a USB webcam), Terco 4426 CNC, Ken-Ton/LR-Tech/TC^3
SCSI (Glenside IDE covers the HDB-DOS/NitrOS-9 need), CoCoIO Ethernet
(never had a fixed register map).

**Not cart-port, out of scope:** CoCoVGA, coco-hdmi, GIME-X (VDG/GIME
sockets), Hi-Res Joystick Interface and Color Mouse (joystick port),
SmartWatch DS1315 (ROM socket), CocoMEM (GIME/CPU sockets).

### 12.3 What this implies for the design

- Every Tier 1 device is register-mapped and fits the response-table
  rule in §8. Self-decoded $FF6x/$FF7x needs Pico-controlled /OE (§1).
- WordPak-RS is the second reason for HDMI after the VDG snoop, and
  the only HDMI feature planned for the CoCo 3 (§4): an 80-column
  NitrOS-9 console on a CoCo 1/2 with no monitor mod. It
  works on the CoCo 3 too (the RS variant was the CoCo 3 version, moved
  to $FF76 because $FF9x became GIME registers): WordPak-targeted
  software (DynaStar, O-PAK) runs unchanged, and NitrOS-9 can drive
  GIME windows on the TV and the WordPak console on HDMI at the same
  time, giving a dual-display CoCo 3.
- Audio outputs: SND pin (SSC, PSG, GMC, Sym12) and a stereo jack
  (Orch-90, Stereo Composer). Mixing everything to both is a config
  option.
- Sources to pull verbatim during implementation: MAME `coco_fdc`,
  `coco_ide`, `coco_multi`, `coco_rs232`, `coco_midi`, `coco_orch90`,
  `coco_ssc`, `coco_psg`, `coco_gmc`, `coco_sym12`, `coco_stecomp`,
  `coco_wpk`, `coco_wpk2p`, `coco_max`, `coco_xsid`, `meb_rtime`, and
  the `dragon_*` files.

---

## 13. Carrier provisions for the Waveshare RP2350B-Plus-W (decided 2026-09-16)

Decision: the main board is designed once, for the RP2350B-Plus-W
footprint. A Pico 2 fits the same footprint and is what the first
build uses. Nothing in this section costs the Pico 2 build anything
beyond unused pads and a jumper left in its default position.

### 13.1 Module facts (Waveshare wiki pinout image, checked 2026-09-16)

- Pico 2 W form factor, 51 x 21 mm, plus a 4.92 mm PCB antenna
  overhang on the end opposite USB-C. RP2350B, 16 MB flash, Raspberry
  Pi Radio Module 2 (RM2, CYW43439), ME6217C33 3.3 V LDO rated
  800 mA, PSRAM footprint unpopulated. About $11.
- The 2x20 header is the Pico 2 pinout for GP0..GP22, RUN, 3V3_EN,
  3V3(OUT), VSYS, VBUS and all grounds. The three ADC positions
  differ: **pins 31, 32, 34 are GP40, GP41, GP42** instead of
  GP26, GP27, GP28. Those are OE_BUS, HALT_GATE and E, so the firmware
  needs a second board header and the core1 wait loop reads the high
  GPIO register for them (bits above 31).
- **15 underside SMD pads** in a 3 x 5 grid under the middle of the
  module carry GP24..GP35 and GP43..GP45. GP23, GP36..GP39, GP46 and
  GP47 are not brought out (radio module and PSRAM CS). The pad pitch
  and coordinates are not published as text; measure a physical
  board or get Waveshare's drawing before laying out the grid.
- The underside pads are only reachable if the module is soldered
  down flat (castellations + hidden pads, paste and hot air), or if a
  3 x 5 header can be soldered to them and the carrier gets matching
  through-holes. Which one depends on the measured pitch.

### 13.2 Power budget

Cart pin 9 is rated +5 V at 300 mA on every model, from the primary
manuals: CoCo 1 (Technical Reference Manual, which also gives +12 V at
300 mA on pin 2), CoCo 2 (NTSC Service Manual 26-3026/3027), CoCo 3
(Service Manual), and the Multi-Pak's own edge card into the CoCo
(MPI Service Manual). The Multi-Pak has its own supply; its per-slot
rating was not checked.

| Load | Figure | Source |
|---|---|---|
| RM2 transmit, MCS7 at 16 dBm | 271 mA | RM2 datasheet |
| RM2 receive active, MCS7 | 43 mA | RM2 datasheet |
| RM2 power-save idle | 1.19 mA | RM2 datasheet |
| Pico 2, one core busy, USB up | 9.4 mA at 5 V | Pico 2 datasheet, CoreMark row |
| Pico 2, USB serial app | 12.7 mA at 5 V | Pico 2 datasheet, hello_usb row |
| PiCoCo board (U14 quiescent, 4 LVC buffers, pull-ups, no LEDs) | 10-15 mA | BOM estimate |

- Pico 2 build: about 30 mA total. Ten times under the rating.
- Plus-W, Wi-Fi idle or receiving: about 90 mA. The Waveshare LDO is
  linear, so its 3.3 V current appears one-for-one on the 5 V rail;
  both cores busy at 150 MHz is an estimate of 25-30 mA.
- Plus-W, transmit burst at full power: about 315 mA for the
  millisecond-scale length of a packet. At or just over the rating.
  300 mA is Tandy's allocation, not a trip point, but do not lean on
  the headroom.

Mitigations, in order: bulk capacitance (13.3), lower transmit power
(the cyw43 driver exposes a tx-power setting; verify the exact call
before relying on it), and Wi-Fi off by default in `picoco.cfg`.
D2 (1 A Schottky), the main board's 0.20 mm power traces (no separate
power netclass as of v2.3 — margin at 1 oz copper is fine at this
current) and the breakout's 0.5 A polyfuse all clear the Plus-W case.

### 13.3 What the carrier gets, and what the Pico 2 build sees

| Provision | Plus-W | Pico 2 build |
|---|---|---|
| Bulk capacitor footprint on `VSYS_PICO` after D2, sized for 1000 uF 6.3 V polymer. On VSYS_PICO, not +5V, so U14 and the buffers never see the burst. Rides a 2 ms transmit burst with ~0.35 V droop. Power-on inrush is a ms spike inside D2's surge rating and too short for the polyfuse. | populated | DNP |
| Antenna keepout: no copper under or beside the 4.92 mm overhang, and clearance for it at that end of the module. | needed | free |
| 3 x 5 underside pad grid (or through-holes, per 13.1). | used | empty |
| Three-pad solder jumper on U10 /OE. Default: U15 output (`OE_BUS`) as today. Alternate: an underside-pad GPIO. Firmware then implements `(cart-selected OR write) AND E` (§1) when it wants to snoop or self-decode. U15 stays on the board because the Pico 2 build has no pins to replace it. | either | default |
| Route existing nets to pads: `CTS_BUF`, `SCS_BUF`, `E_B` (U15's inputs), plus U13's already-buffered `Q_DBG` and `SLENB_DBG` outputs, currently no-connect. | captured | unused |
| A14 and A15 have no buffer channel. U13 spends three channels buffering /HALT, /NMI and /CART as inputs, which `breadboard-plan.md` §2.3 calls pointless. Reassign two of them to A14/A15 in `gen_schematic.py` (a breadboard-plan §8 decision). | full address visible | no change |
| Pin plan: the seven captured inputs (/CTS, /SCS, E, Q, /SLENB, A14, A15) on GP24..GP30 so one 32-bit `gpio_in` read catches them. /OE, /NMI and /CART drives on the remaining pads (GP31..GP35, GP43..GP45). | 10 of 15 pads | n/a |
| Firmware: `boards/` header for the Plus-W with PIN_OE_BUS 40, PIN_HALT 41, PIN_E 42 and the extra pins; core1 wait loop reads `gpio_hi_in` for OE_BUS. | needed | untouched |

HSTX video stays off this path: it needs GP12..GP19, which is the
address block (§1).

### 13.4 Underside pad grid, measured (2026-09-17)

No drawing exists (Waveshare's wiki blocks fetches; the schematic PDF at
files.waveshare.com gives only the pad-to-GPIO map). Measured from two
straight-on photos of the module's underside, using the 40 castellation
holes (2.54 mm pitch, 1.0 mm drill, measured 1.02 mm) as the scale and
frame. Close-up and full-board photos agree to 0.03 mm.

- 3 x 5 pads, **2.54 mm pitch** both ways (measured 2.553 / 2.557),
  **1.5 mm square** (measured 1.52 x 1.50), on the same lattice as the
  header, centred across the board width.
- Columns, in the `RPi_Pico_SMD_TH` footprint frame (pin 1 at
  -8.89, -24.13; pin 20 at -8.89, +24.13; pin 21 at +8.89, +24.13):
  X = -5.08, -2.54, 0, +2.54, +5.08.
- Rows: the row nearest the pin 20/21 end (the antenna end) is
  **1.73 mm** from that hole row (measured 1.72 and 1.74), i.e.
  Y = +22.40, then +19.86, +17.32.
- GPIO on each pad (from the module's silkscreen, matches the schematic
  PDF), listed from the pins-1..20 side to the pins-21..40 side:
  row Y=+22.40: GP26 GP29 GP32 GP35 GP45;
  row Y=+19.86: GP25 GP28 GP31 GP34 GP44;
  row Y=+17.32: GP24 GP27 GP30 GP33 GP43.
- Carrier pads: 1.8 mm square (0.3 mm oversize) on those centres. The
  grid is well inside the module's 21 mm width and 3 pitches from the
  end, clear of the antenna and the USB-C end.

Photos: IMG_8547.jpg (full underside), IMG_8548.jpg (close-up), taken
2026-09-17; measurement script in the session scratchpad
(`measure2.py`, OpenCV, castellation-hole homography/similarity fit).

### 13.5 Open before layout

- Whether the Plus-W can be soldered flat with the hidden pads reliably
  by hand, or whether the header route is needed.
- The cyw43 tx-power call.
- ~~Underside pad pitch~~ — **closed by the v2.3 spec**: 2.54 mm pitch,
  1.5 mm pads (measured §13.4), carried into the `Pico-Carrier`
  footprint as 1.8 mm pads on the same grid (1.4 mm at GP29/GP32/GP35
  to clear the Pico 2's own debug rings).
- ~~Which end of the module faces the board edge~~ — **closed by the
  v2.3 spec**: USB-C end flush with the left side edge, antenna end
  pointing right/inward, fixed this way specifically so a future HDMI
  variant's reserved corner and module orientation don't need to move
  (spec §5, §10).

Sources: Waveshare wiki `RP2350B-Plus-W` (pinout and dimension
images); RM2 datasheet RP-008943; Pico 2 datasheet RP-008299; Pico 2 W
datasheet RP-008304; CoCo 1 Technical Reference Manual (archive.org);
CoCo 2 NTSC Service Manual, Multi-Pak Interface Service Manual
(colorcomputerarchive.com); CoCo 3 Service Manual (archive.org).
