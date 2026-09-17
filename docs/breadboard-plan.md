# PiCoCo - Breadboard-First Plan

Status (2026-09-05): plan approved; section 4 (breakout + cobbler PCBs)
is built and fab-ready in `breakout/` and `fab/`. Sections 2.1 and 2.2
(main-board schematic fixes) are NOT yet applied to `gen_schematic.py`;
they are deliberately deferred to section 8, after the breadboard
confirms them.

Goal: prove the PiCoCo bus interface on a breadboard, driven by a small
cartridge-edge breakout PCB, before routing and fabbing the full v2.2
board. Every step below has a pass/fail check and a way to stop early.

---

## 1. Where the project actually is

| Area | State | Evidence |
|---|---|---|
| Docs (`docs/`) | Complete design narrative, v2.2 | hardware-design, firmware-architecture, BOM, kicad-workflow |
| Schematic | Complete netlist from `tools/gen_schematic.py` | ERC: 2 errors, 22 warnings (see 2.3) |
| PCB | 42 footprints placed, **0 traces, 0 vias, 0 zones** | `grep -c '(segment' PiCoCo.kicad_pcb` = 0 |
| Firmware | **None.** `firmware/` does not exist | The architecture doc is a plan, not code |
| Fab outputs | `fab/` has only the checklist, no Gerbers | |
| Git | All docs, tools, LICENSE, CLAUDE.md uncommitted | `git status` |

So "walk before you run" is the right call: the remaining work is
routing (a day or two) plus fab lead time plus the entire firmware,
and any wiring mistake costs a board spin. Two such mistakes already
exist (next section).

## 2. Design review findings

### 2.1 U10 data-bus transceiver is wired to the wrong rails (fab-blocking)

`gen_schematic.py` puts the CoCo 5 V data lines on the **A port**
(referenced to VCCA = +3V3) and the Pico on the **B port** (VCCB =
+5V). On the SN74LVC8T245 each port is referenced to its own rail, so
as wired the chip would drive **5 V into the Pico's GPIOs** on every
CoCo write, and would drive the CoCo bus at only 3.3 V levels on reads
(harmless, but not what the doc says).

Fix (one edit in `u10_nets`): swap the port assignments so the Pico
D0-D7 sit on A1-A8 (pins 3-10) and the cart D0-D7 sit on B1-B8 (pins
21-14). Rails stay as they are. With that swap, `DIR = R/W` is correct:
R/W high (CoCo read) selects A-to-B, i.e. Pico-to-CoCo.

### 2.2 U15 /OE gate has the wrong polarity on E (fab-blocking)

U15 is a 3-input AND: `OE_BUS = CTS_BUF AND SCS_BUF AND E_B`. An AND
output goes low when **any** input is low, so `OE_BUS` is asserted
during every E-low phase of every bus cycle, cart selected or not.
U10 would be enabled about 50% of the time the CoCo is talking to its
own RAM.

The intended function is: enable only when (/CTS low OR /SCS low) AND
E high. That is `/OE = NAND( NAND(CTS, SCS), E )`, two 2-input NAND
gates. On the final PCB that is one 74LVC2G00 (or two 1G00s). On the
breadboard it is half a 74HC00.

### 2.3 Smaller items to carry into the doc/generator cleanup

- ERC now reports **2** errors, not the 1 the docs promise. The new one
  is `VSYS_PICO` with no power driver (D2's cathode is a passive pin).
  A `PWR_FLAG` on `VSYS_PICO` fixes it.
- `A0_B`..`A13_B` use the `_B` suffix that CLAUDE.md says is banned.
  They are real, connected nets, so this is a naming inconsistency,
  not the orphan-net bug the rule guards against. Rename to `_BUF` or
  amend the rule.
- HDB-DOS is an **8 KB** ROM ($C000-$DFFF). The docs say 16 KB in
  several places. The A13-as-selector scheme only works with the 8 KB
  interpretation, which is what the firmware doc actually assumes.
- Becker decode must match **A5..A13 too**, not just A0..A4. A /CTS
  read anywhere in $E000-$FEFF has A13 = 1 and would otherwise be
  mistaken for a Becker cycle. Matching the full 14-bit pattern
  `%11 1111 010x xxxx` disambiguates because A14/A15 are implied high
  inside the cart windows.
- /SLENB (pin 40) and /CART (pin 8) are **cart-to-CoCo** signals. Buffering
  them as inputs is harmless but pointless. R6/JP1 (a /CART pull-up)
  does nothing useful: HDB-DOS autostarts via the "DK" signature check
  at $C000, not via /CART. Candidate for deletion after the breadboard
  confirms it.
- Tying Pico RUN to CoCo /RESET **creates** the boot race the /HALT
  circuit then has to solve (every reset button press reboots the Pico
  and drops the USB CDC link). The breadboard should measure whether
  either circuit is needed (step 8 below) before both are committed to
  copper.
- The cart footprint `COCO-CART-2.1X1.75` ends its fingers 0.44 mm from
  the board edge. JLCPCB's order form gives the 30° bevel on 1.6 mm
  stock as 1.13 mm deep, so the bevel would cut into the finger tips.
  The breakout's derived footprint pulls the leading edge back to
  1.30 mm; do the same to the main footprint before it is fabbed.
- Firmware doc 3.4: DMA cannot index a table by a value pulled from
  the PIO FIFO. The workable version has the PIO compose the full
  32-bit pointer itself (`in pins, 13` then `in y, 19` with Y holding
  the 8 KB-aligned ROM base) and a single DMA channel that writes that
  word into a second channel's READ_ADDR_TRIG. Not needed for
  breadboard bring-up; a tight C loop is faster to get working (7.2).

## 3. Strategy

One tiny new PCB (cart-edge breakout, hard gold fingers) plus DIP
substitutes for every SMD part on a breadboard. The Pico and all logic
live on the breadboard, so any topology change is a wire move.

```
CoCo slot ──[breakout PCB]──40-way IDC ribbon──[PiCoCo-Cobbler]──┐
                                                                 │
   breadboard: Pico 2 · 4x 74LVC245AN · 74HC00 · LD1117V33 · 2N7000
```

The breadboard end is a second tiny PCB, `PiCoCo-Cobbler`, generated by
the same script. **Do not use a Raspberry Pi T-Cobbler here.** Adafruit's
Eagle schematic shows it buses all eight GND pins and both 5V pins on
the adapter, which would short E, +5V, D4, A1, A6, A11, GND and A15
through the ribbon. The PiCoCo-Cobbler is strictly 1:1 and keyed.

## 4. Breakout PCB spec

Two KiCad projects in `breakout/`, both written by `tools/gen_breakout.py`
(run with KiCad's bundled Python; it uses the pcbnew API to route and
pour): `PiCoCo-Breakout` (fingers to header) and `PiCoCo-Cobbler`
(header to breadboard). The fingers-only footprint and symbol
`COCO-CART-FINGERS` are derived from the existing cart footprint.
Status 2026-09-05: generated, ERC/DRC clean, Gerbers in `fab/breakout/`
and `fab/cobbler/`. See `breakout/README.md`.

| Ref | Part | Notes |
|---|---|---|
| P1 | COCO-CART-2.1X1.75 fingers | existing footprint |
| J1 | 2x20 shrouded IDC box header (keyed; overhangs the 52.8 mm board by 2.8 mm each side) | pin n = finger n; header 1,2 = GND |
| F1 | 0.5 A polyfuse, 1812 SMD (Bourns MF-MSMF050-2) | in series with +5V (pin 9) before J1; a breadboard slip must not short the CoCo's 5 V rail |
| C1 | 10 uF + 100 nF | on +5V after F1 |
| D1 + R1 | LED + 1 kΩ | power indicator, optional |

Rules:
- Fingers 1 and 2 (-12 V / +12 V on CoCo 1/2) are **left open**, so
  ±12 V can never reach a breadboard. Header pins 1 and 2 are GND.
- Everything else 1:1. No pull-ups, no logic. Keep it dumb.
- Board 52.8 x 99 mm (header 77 mm from the finger tip; the case surface
  is at ~55 mm on a CoCo 3), 1.6 mm, hard gold fingers, 30° bevel, per
  `fab/READ-BEFORE-ORDERING.txt`. JLCPCB minimum is 5 boards; spares
  are useful.
- Before fabbing, check whether a commercial CoCo cartridge breakout is
  in stock (Tindie / CoCo forums). If one exists at a sane price it
  removes a two-week wait.

## 5. Breadboard BOM (DIP substitutes)

| Final-PCB part | Breadboard part | Why |
|---|---|---|
| Pico 2 (SMD module) | Pico 2 with headers | RP2350 preferred: faster, more SRAM. RP2040 works too at CoCo 1/2 speed. |
| U10 SN74LVC8T245 (SOIC-24) | **SN74LVC245AN** (DIP-20) at 3.3 V, or 8T245 on a SOIC-24 adapter | No DIP 8T245 exists. An LVC245 at 3.3 V has 5 V-tolerant inputs and its 3.3 V VOH is a valid TTL high for the 6809/SAM/PIA. If it works, the 8T245 may not be needed on the PCB either. |
| U11, U12, U13 SN74LVC245AD | SN74LVC245AN (DIP-20) x3 | same part number family |
| U15 SN74LVC1G11 | **74HC00** (DIP-14) at 3.3 V | implements the corrected NAND-NAND decode (2.2); ~25 ns tpd at 3.3 V is negligible |
| U14 AMS1117-3.3 | LD1117V33 (TO-220) + 10 uF in / 22 uF out | breadboard-friendly |
| D2 SS14 | 1N5819 | |
| Q2 2N7002 | 2N7000 (TO-92) | |
| R1..R14 | same values, 1/4 W through-hole | R2, R3, R6, JP1 omitted (DNP / YAGNI) |
| C4..C10 | 100 nF ceramic, one per IC, as close to the pin as a breadboard allows | |

Tools worth having before step 2:
- 8-channel USB logic analyzer (24 MSa/s Saleae-clone class, ~$10).
  The CoCo bus is 0.89 or 1.79 MHz; this is more than enough and it
  is the single most useful bring-up tool here.
- Two 830-point breadboards. Pico + five DIPs + cobbler will not fit
  on one.
- 40-way IDC ribbon, 20-30 cm, female IDC both ends. Keep it short.
- PiCoCo-Cobbler board + one 2x20 box header + two 1x20 pin headers.
- Raspberry Pi Debug Probe (optional; USB BOOTSEL flashing is fine).

## 6. Bring-up sequence

Each step is a gate. Do not move on until the check passes. Steps 1-2
touch no Pico and cannot damage anything; step 6 is the first moment
the Pico drives the CoCo bus. The "Console" column names the
`pconsole.py` commands to run at that step; see `firmware/README.md`
"Bring-up" for the full expected transcripts and the three debugging
tools (trace dump, dw capture, status counters).

| # | Do | Console | Pass check | Firmware tag |
|---|---|---|---|---|
| 1 | Breakout alone in the slot, nothing on the ribbon | (none) | CoCo boots to BASIC normally; +5 V present at the cobbler | - |
| 2 | Logic analyzer on E, Q, R/W, /CTS, /SCS, A13 at the cobbler (5 V signals, LA is 5 V tolerant) | (none) | `PEEK(&HC000)` shows a /CTS pulse; `PEEK(&HFF41)` shows /SCS. Record whether /CTS and /SCS are already E-qualified on your machine and the exact E-to-select timing. This decides how much of the U15 gate is really needed. | - |
| 3 | LDO + 1N5819 + Pico on the breadboard, powered only from the CoCo | (none) | Pico powered from the CoCo rail: LED blinks 1 Hz; `status` over USB. USB power and CoCo 5 V share only GND on the breadboard (VBUS is NC on the final board): do not back-power the CoCo from USB, use a USB cable with VBUS cut, or accept that the Pico is powered by USB during console sessions. | fw-0.1-blink |
| 4 | Input buffers only (U11/U12/U13 equivalents) into GP8-GP22, GP28. Pico GP0-7 left as inputs. | `bus drive off`, `trace run`, on the CoCo `PEEK(&HC123)`, then `trace dump 8` | Dump shows an entry `0123 R ff` (idx = $C123 - $C000); `status` bus reads incremented by the number of PEEKs; `PEEK(&HFF41)` shows `3f41 R`. Pico still drives nothing. | fw-0.4-bus-capture |
| 5 | 74HC00 decode: NAND(CTS,SCS) then NAND(that, E) → OE_BUS → GP26 | (hardware only) | LA: OE_BUS low only during E-high of cart cycles, never otherwise | - |
| 6 | Data buffer (U10 equivalent) between GP0-7 and the cart D0-7, /OE from step 5, DIR from RW_BUF. | `rom pattern`, `bus drive on`, `save` | `PEEK(&HC000)` = 0, `PEEK(&HC001)` = 1, `FOR I=0 TO 255: PRINT PEEK(&HC000+I);: NEXT` counts up. Run on the slowest CoCo you have first; on CoCo 3 repeat after `POKE 65497,0`. | fw-0.5-rom-static |
| 7 | Load a real HDB-DOS/DriveWire 8 KB image | `fs export`, copy `hdbdos_dw.rom` (8 KB), `fs import`, `rom load hdbdos_dw.rom`, `save` | Power-cycle: CoCo autostarts HDB-DOS (or `DOS` enters it); `DIR` fails cleanly (no server yet) | fw-0.6-rom-hdbdos |
| 8 | **Optional (decided 2026-09-17: Q2/R7/R8 stay on the PCB regardless, see hardware-design §4.4).** Experiment, not a build step: measure time from /RESET release to the first $C000 read, and separately the Pico's cold-boot time to "core1 up, halt released" | `log main debug`; LA on /RESET and $C000 | Compare the two times; decide Q2/R7/R8 per section 2.3. If Pico boot is comfortably shorter, drop Q2/R7/R8 and the RUN-from-/RESET path and free GP27. If not, add the 2N7000 circuit now and re-check. | fw-0.3-halt-ctrl |
| 9 | Becker loopback: $FF41 returns $02, $FF42 echoes last write | `becker loop`, `bus drive on` | `POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` prints `0 2 65` and `trace dump` shows `3f42 W 41` (the first `PEEK(&HFF41)` always returns the pre-POKE status: core1 refreshes the table only after a read, single-writer rule; check the `W` byte, not the screen, for bit order) | fw-0.7-becker-loop |
| 10 | **Deferred 2026-09-17 to ADDITIONAL_ROADMAP §5; native mode (step 11) passed first.** Becker ↔ USB CDC bridge; pyDriveWire on the host pointed at the Pico's serial port | `becker bridge`; host: `pyDriveWire --port /dev/tty.usbmodemXXXX1 --speed 115200 <image>` (the CDC0/bridge port) | `DIR` in HDB-DOS lists the image; `LOADM` a small program | fw-0.8-bridge |
| 11 | Full native DriveWire over the same bridge, no host-side pyDriveWire process | `fs export`, copy a DSK, `fs import`, `dw mount 0 <dsk>`, `becker native`, `save` | `DIR`, `LOADM`, `SAVE` a program, power-cycle, `DIR` still shows it; `dw stats` shows reads/writes, `crc_err 0`, `timeouts 0` | fw-1.0-native |

Optional side experiment after step 7 (cheap, could shrink the BOM):
the RP2350 datasheet describes its digital GPIOs as 5 V tolerant while
IOVDD is powered, with the ADC-capable pins GP26-29 excluded. Verify
that claim in the current datasheet revision first. If it holds, try
feeding A0-A13 and R/W to the Pico directly from the cobbler with no
LVC245s, keeping only the data buffer and the decode gate. E would
need to move off GP28 (an ADC pin) for that test. This is a design
change and your call; the baseline plan keeps the buffers.

## 7. Firmware: minimum path

### 7.1 Skeleton
`firmware/` with Pico SDK ≥ 2.1, CMake, one `main.c`. USB CDC stdio
for logs. Tag each milestone as the table above.

### 7.2 First ROM server: a loop, not PIO
For steps 4-7 use core1 in a tight loop with interrupts off:

```
wait until OE_BUS (GP26) low
snap = gpio_get_all()
if A13 == 0 and R/W == 1:  set GP0-7 output, write rom[snap & 0x1FFF]
wait until OE_BUS high
set GP0-7 input
```

At 150 MHz that is well under 100 ns from OE-low to data valid, inside
the 280 ns CoCo 3 double-speed window and far inside the CoCo 1/2
window. It is also the easiest thing to debug with `printf` on core0.
Move to PIO + DMA (with the pointer-composing fix from 2.3) only if
the loop shows jitter on the LA.

### 7.3 Becker
Same loop, add the A13 = 1 branch with the full-address match from
2.3. Two ring buffers, CDC pump on core0.

## 8. Folding results back into the main board

After step 10 passes:

1. Apply 2.1 and 2.2 to `tools/gen_schematic.py`, plus the PWR_FLAG
   and naming cleanups in 2.3. Regenerate, ERC, expect exactly 1
   (known) error.
2. Decide from step 6/8/side-experiment evidence: keep 8T245 or use
   LVC245 for data; keep or drop Q2 + RUN-from-/RESET; keep or drop
   R6/JP1; keep or drop U11-U13.
3. Update `docs/hardware-design.md`, `docs/firmware-architecture.md`,
   and CLAUDE.md hard rules to match what was proven.
4. `Update PCB from Schematic`, `place_pcb.py`, route, zones, DRC in the
   GUI, then `gen_fab.sh`.

## 9. Assumptions made in this plan

- You have at least one CoCo to test against; which model is unknown,
  so timing claims are stated for the slower 0.89 MHz case first.
- You can source an HDB-DOS + DriveWire 8 KB binary (Cloud9, or built
  from the toolshed sources).
- Fab is JLCPCB or similar with a hard-gold-finger option, as the
  existing checklist assumes.
- The v2.2 architecture (buffered bus, hardware /OE gate, A13 decode)
  is what we are validating. The breadboard may show parts of it are
  unnecessary; those become decisions in section 8, not silent
  changes.
