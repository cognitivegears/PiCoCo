# PiCoCo main board v2.3: carrier for Pico 2 / RP2350B-Plus-W, through fab order

Date: 2026-09-17. Status: design approved in conversation; this document is
the written spec. Implementation plan follows separately.

## 1. Goal

One PCB, ordered from JLCPCB with SMT assembly, that:

- boots the CoCo 1/2/3 into HDB-DOS from a ROM the Pico serves and runs
  native DriveWire over the Becker port, exactly as the breadboard now
  does (tags `fw-0.6-rom-hdbdos`, `fw-1.0-native`);
- takes either a Raspberry Pi Pico 2 (first build) or a Waveshare
  RP2350B-Plus-W soldered flat, with the Plus-W's 15 underside pads
  connected for the roadmap features, at no cost to the Pico 2 build;
- is hand-buildable by a hobbyist with an iron (no 0.65 mm pitch parts,
  no hidden pads required for the Pico 2 build);
- ships as an order-ready package: Gerbers, JLCPCB BOM and CPL with
  LCSC numbers, a module stencil, and a checklist with the exact order
  options.

Out of scope for this spin: firmware support for the Plus-W pins (a
`boards/` header, later), a Program Pak shell-sized board (roadmap; the
only known-good outline is 53.34 x 44.45 mm, NF6X CoCoEPROMpak), bridge
mode (ADDITIONAL_ROADMAP §5), /RESET drive into the CoCo.

## 2. Decisions carried into this spec (with dates)

| Decision | Date | Where recorded |
|---|---|---|
| One carrier for Pico 2 and Plus-W; Plus-W provisions as DNP/pads/jumper | 2026-09-16 | RP2350B_IDEAS §13 |
| Q2/R7/R8 (/HALT hold) stay; step 8 optional | 2026-09-17 | hardware-design §4.4 |
| R9 populated: RUN tied to CoCo /RESET; reset reason feeds video shadow | 2026-09-17 | hardware-design §4.5 |
| U10 becomes 74LVC245A at 3.3 V (proven on bench at 0.89 and 1.79 MHz) | 2026-09-17 | this spec |
| Decode is NAND-NAND (proven on bench with a 74HC00) | 2026-09-15 | breadboard-plan §2.2 |
| Hand-build friendly packages: SOIC, SOT-23, SOT-223, SMA, 0805 | 2026-09-17 | this spec |
| Hard gold fingers + 30° bevel default; ENIG documented as budget option | 2026-09-17 | this spec |
| Bare 98 x 55 mm board; shell fit is a roadmap board | 2026-09-17 | this spec |
| Plus-W mounted flat (castellations + hidden pads); Pico 2 flat or on 2x20 headers | 2026-09-17 | this spec |
| Underside pad grid: 2.54 mm pitch, 1.5 mm pads, 1.73 mm from the pin 20/21 row | 2026-09-17 (measured) | RP2350B_IDEAS §13.4 |

## 3. Schematic v2.3 (`tools/gen_schematic.py`)

All changes are edits to the declarative `pin_nets` blocks and the
`SYMBOLS` list. The generator stays the single source; the `.kicad_sch`
is regenerated.

### 3.1 Net naming

- Buffered signals end in `_BUF`: `A0_BUF`..`A15_BUF`, `RW_BUF`,
  `CTS_BUF`, `SCS_BUF`, `E_BUF`, `Q_BUF`, `SLENB_BUF`, `RESET_BUF`.
  The `_B` suffix and `RESET_DBG` disappear.
- `_RAW` stays for the pre-termination side of R11/R12:
  `OE_BUS_RAW`, `RW_BUF_RAW`.
- Cart-side nets keep `_CART`.
- New nets: `SEL_N` (first NAND output), `OE_FW` (pad-grid GPIO to JP2),
  `NMI_DRV`, `CART_DRV` (pad-grid GPIOs to the DNP FET stages),
  `PICO_P34` (module header pin 34, JP3 centre), `AUDIO_PWM`,
  `AUDIO_F1`, `AUDIO_F2` (filter nodes), `SND_CART` (cart pin 35).

### 3.2 Parts and connections

**U1 module** (`PiCoCo:Pico` symbol, extended): header pins as today,
plus 15 new pins numbered by GPIO name (`GP24`..`GP35`, `GP43`..`GP45`).
Header pins 31/32/34 are labelled `GP26/GP40`, `GP27/GP41`, `GP28/GP42`
in the symbol so the Plus-W mapping is visible.

| Pad-grid pin | Net | Purpose (Plus-W only; NC on a Pico 2) |
|---|---|---|
| GP24 | CTS_BUF | capture |
| GP25 | SCS_BUF | capture |
| GP26 | E_BUF | capture (same net also on header pin 34) |
| GP27 | Q_BUF | capture |
| GP28 | SLENB_BUF | capture |
| GP29 | A14_BUF | capture |
| GP30 | A15_BUF | capture |
| GP31 | OE_FW | JP2 alternate: firmware-driven U10 /OE |
| GP32 | NMI_DRV | Q3 gate (DNP stage) |
| GP33 | CART_DRV | Q4 gate (DNP stage) |
| GP34 | AUDIO_PWM | sound output stage (§3.2 Sound) |
| GP35, GP43, GP44, GP45 | no-connect | spare |

GP24..GP30 are contiguous so one `gpio_in` read captures all seven.

**U10 data buffer**: `74xx:74LS245` symbol, value `SN74LVC245A`,
footprint SOIC-20W. Pin 1 DIR = `RW_BUF`. Pins 2..9 (A1..A8) = Pico
`D0`..`D7`. Pins 18..11 (B1..B8) = `D0_CART`..`D7_CART`. Pin 19 /OE =
`U10_OE` (JP2 centre). Pin 20 = `+3V3`, pin 10 = `GND`. With R/W high
(CoCo read) DIR=1 drives A to B, Pico to CoCo. C11 (the 8T245's 5 V bulk)
is deleted.

**U11, U12, U13 address/control buffers**: same symbol, value and
footprint as U10. DIR = `+3V3`, /OE = `GND`.

- U11: A1..A8 = `A0_CART`..`A7_CART`; B1..B8 = `A0_BUF`..`A7_BUF`.
- U12: A1..A6 = `A8_CART`..`A13_CART`, A7 = `RW_CART`, A8 = `CTS_CART`;
  B1..B6 = `A8_BUF`..`A13_BUF`, B7 = `RW_BUF_RAW`, B8 = `CTS_BUF`.
- U13: A1 = `SCS_CART`, A2 = `E_CART`, A3 = `Q_CART`, A4 = `SLENB_CART`,
  A5 = `RESET_CART`, A6 = `A14_CART`, A7 = `A15_CART`, A8 = `GND`;
  B1 = `SCS_BUF`, B2 = `E_BUF`, B3 = `Q_BUF`, B4 = `SLENB_BUF`,
  B5 = `RESET_BUF`, B6 = `A14_BUF`, B7 = `A15_BUF`, B8 no-connect.
  /HALT, /NMI, /CART are no longer buffered as inputs.

**U15 decode**: `74xx:74LS00` symbol, value `SN74LVC00A`, SOIC-14.
Gate 1: `CTS_BUF`, `SCS_BUF` -> `SEL_N`. Gate 2: `SEL_N`, `E_BUF` ->
`OE_BUS_RAW`. Gates 3 and 4: inputs to `GND`, outputs no-connect.
VCC = `+3V3`, GND. `OE_BUS = NAND(NAND(CTS,SCS), E)`: low only while a
cart address is selected and E is high. R11 33 R: `OE_BUS_RAW` ->
`OE_BUS`. `OE_BUS` fans out to JP2 pad 1, Pico header pin 31, TP1.

**JP2 solder jumper** (`Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm`):
pad 1 = `OE_BUS`, pad 2 (centre) = `U10_OE`, pad 3 = `OE_FW`. Pads 1-2
bridged in copper by default. A Plus-W user cuts 1-2 and bridges 2-3 to
let firmware drive U10 /OE.

**/HALT drive**: unchanged. Q2 2N7002, R8 100 R gate series from
`HALT_GATE` (header pin 32), R7 100 k gate pull-up to `+3V3`, drain
`HALT_CART`, R1 4.7 k pull-up of `HALT_CART` to `+5V`.

**/NMI and /CART drive, DNP**: Q3 and Q4 2N7002 with R15/R16 100 R gate
series from `NMI_DRV`/`CART_DRV` and R17/R18 100 k gate pull-downs to
`GND` (released when nothing drives them). Drains to `NMI_CART` and
`CART_CART`. R2 4.7 k `NMI_CART` pull-up stays DNP. R6 and JP1 are
deleted (no /CART pull-up; HDB-DOS autostarts on the DK signature).

**Reset**: U13 B5 `RESET_BUF` -> R9 100 R -> `PICO_RUN`; R10 10 k pull-up
to `+3V3`. R3 4.7 k `RESET_CART` pull-up stays DNP.

**Sound** (cart pin 35 = `SND_CART`, an analog input the CoCo mixes to
its audio when `AUDIO ON` selects the cart). Populated by default, all
Basic 0805 parts, inert when nothing drives it.

- JP3 solder jumper (same footprint family as JP2): pad 1 = `E_BUF`,
  pad 2 (centre) = `PICO_P34` = module header pin 34 (GP28 on a Pico 2,
  GP42 on a Plus-W), pad 3 = `AUDIO_PWM`. Pads 1-2 bridged by default,
  so header pin 34 carries E as today. On a Pico 2 build, where no GPIO
  is spare, cut 1-2 and bridge 2-3 to turn GP28 into the PWM audio
  output; the firmware does not use E (OE_BUS is E-qualified in
  hardware; E was reserved for the PIO engine, a Plus-W path).
- Plus-W: pad-grid GP34 -> `AUDIO_PWM` directly, JP3 stays default.
  (Do not set JP3 to 2-3 on a Plus-W with GP34 populated: two outputs on
  one net. Silkscreen says so.)
- Stage: `AUDIO_PWM` -> R19 1 k -> `AUDIO_F1` -> C13 10 nF to GND ->
  R20 1 k -> `AUDIO_F2` -> C14 10 nF to GND (two-pole RC, about 16 kHz)
  -> R21 2.2 k -> `SND_CART`; R22 1 k from `SND_CART` to GND. DC-coupled
  divider to about 1 V full scale, the way the Orchestra-90 fed the
  same input. R21/R22 are the level knob: tune on the first board
  against a real CoCo and record the values. C15 footprint (0805) in
  parallel with R21, DNP, in case AC coupling is preferred.
- What this enables: any sound the Pico synthesizes itself, on both
  builds, driven by software that uses PiCoCo's own registers in the
  /SCS window (a PSG at unused $FF5x addresses, DriveWire-side sound
  commands, streamed audio). Emulating existing sound carts by
  capturing their writes (Orchestra-90 $FF7A/B, Speech/Sound Pak
  $FF7D/E) is Plus-W only: those addresses are outside /CTS and /SCS,
  so they need JP2 in the firmware-/OE position and A14/A15 from the
  pad grid. TP7 on `SND_CART`.

**Power**: unchanged topology. `+5V` from cart pin 9; D2 SS14 to
`VSYS_PICO`; U14 AMS1117-3.3 to `+3V3` (C2 10 uF in, C3 22 uF out); C1
10 uF on `+5V`; R4 10 k `PICO_3V3_EN` pull-up to `VSYS_PICO`. New: C12
1000 uF 6.3 V SMD electrolytic on `VSYS_PICO`, DNP (Plus-W Wi-Fi burst).
`PWR_FLAG` on `VSYS_PICO` (fixes the review's second ERC error) and on
`+5V`/`GND` as needed for zero-warning ERC apart from the known
COCO-CART pin_to_pin error.

**Decoupling**: 100 nF per IC: C4..C10 for U10..U15 (six ICs, plus one
spare footprint next to the module's 3V3_EN/VSYS pins).

**Debug**: J_SWD 1x4 (SWCLK via R13, SWDIO via R14, GND, +3V3). TP1..TP7
on `OE_BUS`, `RW_BUF`, `CTS_BUF`, `SCS_BUF`, `E_BUF`, `+3V3`, `SND_CART`.

**Fiducials**: FID1, FID2 (`Fiducial:Fiducial_1mm_Mask2mm`), no net,
excluded from BOM.

### 3.3 Part properties for ordering

Every placed symbol carries `LCSC` and `MPN` properties (blank on DNP
mechanical parts). Values from the 2026-09-17 lookup:

| Ref | MPN | Package | LCSC | JLC library |
|---|---|---|---|---|
| U10..U13 | SN74LVC245ADWR (or Nexperia 74LVC245AD if cheaper at order) | SOIC-20W | C571201 | Extended |
| U15 | SN74LVC00AD (or Nexperia 74LVC00AD) | SOIC-14 | to confirm at order | Extended |
| U14 | AMS1117-3.3 | SOT-223 | C6186 | Basic |
| Q2, Q3, Q4 | 2N7002 | SOT-23 | C8545 | Basic |
| D2 | SS14 | SMA | C2480 | Basic |
| R 33 R / 100 R / 4.7 k / 10 k / 100 k | 0805 1 % | 0805 | C17634 / C17408 / C17673 / C17414 / C17407 | Basic |
| C 100 nF / 10 uF / 22 uF | X7R 50 V / X5R 25 V / X5R 25 V | 0805 | C49678 / C15850 / C45783 | Basic |
| R19, R20 1 k; R21 2.2 k; R22 1 k; C13, C14 10 nF X7R | audio stage | 0805 | confirm C numbers at order (all Basic values) | Basic |
| C12 (DNP) | 1000 uF 6.3 V SMD electrolytic, 8 x 10 mm | D8 | pick at order (C970711 had 40 in stock) | n/a |

Lifecycle: all ICs Active at TI/Nexperia; discretes and passives
commodity. `docs/BOM.md` is rewritten from this table (its old LCSC
numbers C9963, C35952, C130103 were wrong).

### 3.4 Generator self-check

`gen_schematic.py` gains a `check()` run after `build()`, assert-based,
that fails the regeneration if:

- any net has exactly one pin (typo detector), excluding declared
  no-connects and power flags;
- any net name ends in `_B` or contains `_DBG`;
- U10 pins 2..9 are `D0`..`D7` in order and pins 18..11 are
  `D0_CART`..`D7_CART` in order;
- U15 gate inputs are `CTS_BUF`,`SCS_BUF` and `SEL_N`,`E_BUF`;
- JP3 pad 1 is `E_BUF` and pad 2 is the module's header pin 34 net;
- every non-DNP, non-mechanical symbol has a non-empty `LCSC`.

`tools/gen_breakout.py` imports from the generator; it must still run
and produce an identical breakout (diff the generated files).

## 4. Library

### 4.1 Module footprint `PiCoCo:RPi_Pico_SMD_TH`

- Keep the 40 THT+castellation pads and the SWD pads.
- Add 15 SMD pads, 1.8 x 1.8 mm, F.Cu/F.Mask, numbered `GP24`..`GP45`
  per RP2350B_IDEAS §13.4, at X = -5.08, -2.54, 0, +2.54, +5.08 and
  Y = +22.40 (GP26 GP29 GP32 GP35 GP45), +19.86 (GP25 GP28 GP31 GP34
  GP44), +17.32 (GP24 GP27 GP30 GP33 GP43), listed from the pin-1..20
  side (X<0) to the pin-21..40 side.
- **No F.Paste on any module pad** (castellation rectangles, the 15 grid
  pads). The module is never factory-placed; paste there would leave
  bumps under it.
- Courtyard extended to the Plus-W envelope: 55.92 mm long, the extra
  4.92 mm beyond the pin 20/21 end (Y = +25.5 .. +30.42 in the footprint
  frame), marked on F.Fab as "antenna keepout".
- Silkscreen: pin 1 marker, "USB" at the USB end, "ANT" at the other.

### 4.2 Cart footprint `PiCoCo:COCO-CART-2.1X1.75`

Finger pads shortened so they end 1.30 mm from the board edge (as
`COCO-CART-FINGERS` on the breakout already does), so JLCPCB's 1.13 mm
30° bevel does not cut into them. Nothing else changes.

### 4.3 Symbol `PiCoCo:Pico`

Add the 15 pins (`GP24`..`GP45`), passive, on a new right-hand column;
relabel header pins 31/32/34 as above. The breakout generator does not
use this symbol.

## 5. PCB layout

- Outline unchanged: 98 x 55 mm, fingers along the bottom edge, MTG1.
- Module U1 horizontal along the top edge, rotated so the USB-C end is
  flush with one side edge (reachable with the cart inserted) and the
  antenna end lies inside the board. A rule area (no copper, both
  layers, no vias) covers the antenna zone from the module's pin 20/21
  end to 4.92 mm past it, full module width.
- Top side under the module: no components (flat mount has zero
  clearance). Traces and vias are allowed under it on both layers
  except inside the antenna rule area.
- All SMT on the top layer (single-side economic assembly). Bottom
  layer: traces and the ground pour only.
- Row nearest the fingers: U10..U13 in signal order (data buffer nearest
  D0..D7, address buffers under A0..A15), U15 next to U12/U13 outputs.
  Short, direct paths from fingers to buffers; buffers to the module.
  Series R11/R12 next to their buffer outputs.
- U14, D2, C1..C3, C12 footprint near the +5V finger (pin 9); R4 near
  the module's 3V3_EN.
- Q2 and R1 near the /HALT finger (pin 3); Q3/Q4 stages near /NMI (4)
  and /CART (8). JP2 next to U10 pin 19. Audio stage R19..R22, C13..C15
  in a straight line ending at the SND finger (pin 35), away from the
  buffers' switching edges; JP3 next to the module's pin 34.
- Module orientation fixed for the future HDMI variant (§10): USB-C end
  flush with the LEFT side edge, antenna end pointing right. The
  top-right corner, about 25 x 15 mm, is reserved: no components, no
  routing denser than needed to pass, silkscreen "HDMI (future)". It
  sits about 15 mm from the antenna rule area and on the top edge so a
  plug clears neighbouring Multi-Pak slots.
- J_SWD and TP1..TP6 along the top edge between the module and the
  reserved corner, reachable with the board in the slot. FID1/FID2 in
  two diagonal corners, 3 mm in.
- Silkscreen: references, "PiCoCo v2.3", CERN-OHL-S-2.0, project URL,
  "JLCJLCJLCJLC" placeholder for the order number, JP2 legend
  ("1-2 = HW /OE default, 2-3 = FW"), JP3 legend ("1-2 = E default,
  2-3 = audio on GP28, Pico 2 only"), D2 cathode band, C12 polarity,
  module "USB"/"ANT" arrows.
- Net classes as in `PiCoCo.kicad_pro`: 0.2 mm signal, 0.5 mm power
  (+5V, +3V3, VSYS_PICO, GND), 0.15 mm clearance, 0.3 mm edge
  clearance. Ground pour both layers, stitched.
- `tools/place_pcb.py` `PLACEMENT` updated for the new set of refs and
  the module rotation; run once, then route in the KiCad GUI.

## 6. Fab package (`tools/gen_fab.sh`)

Outputs under `fab/main/`:

1. Gerbers + drill as today, F.Paste = all placed SMT pads, none on U1
   (falls out of 4.1).
2. `PiCoCo-main-stencil-module.gbr`: F.Paste variant with only U1's
   castellation and grid pads, produced from a temporary copy of the
   board with U1's pads given an F.Paste layer (script does the toggle;
   nothing changes in the checked-in files).
3. `PiCoCo-main-BOM-jlc.csv`: columns Comment, Designator, Footprint,
   LCSC Part #, MPN; grouped; DNP excluded (`kicad-cli sch export bom
   --fields ... --group-by ... --exclude-dnp`).
4. `PiCoCo-main-CPL-jlc.csv`: Designator, Mid X, Mid Y, Layer, Rotation,
   from `kicad-cli pcb export pos`, with a rotation-offset table applied
   per footprint (SOIC-20W, SOIC-14, SOT-23, SOT-223, SMA, 0805) after
   one check in JLCPCB's placement preview; the table lives in the
   script with the date it was verified.
5. `fab/main/READ-BEFORE-ORDERING.txt` (replaces the current text):
   JLCPCB options: FR-4, 2 layers, 1.6 mm, 1 oz, surface finish ENIG
   (so the non-finger pads are gold too and the fingers get no HASL),
   gold fingers YES, bevel 30°, remove order number = specify location,
   SMT assembly top side, economic, BOM/CPL files, confirm the two
   Extended parts, DNP list (R2, R3, C12, Q3, Q4, R15..R18, FID*),
   module not included. Budget variant: ENIG without gold fingers and
   without bevel, for hobbyists ordering bare boards. Plus-W notes:
   hidden pads need the module stencil and hot air or a paste syringe;
   header mounting loses the 15 GPIOs. PCBWay: same files, MPN column.

`gen_fab.sh` keeps working for the breakout boards (path argument).

## 7. Documentation updates in the same change set

- `docs/hardware-design.md`: block diagram, §2 component list, §3.2
  GPIO map (add the Plus-W column and the pad-grid table), §4.1 (LVC245
  at 3.3 V, ports), §4.2 (U13 channels), §4.3 (NAND-NAND with the
  74LVC00), §7 (fiducials), §9 (drop the items now provisioned).
- `docs/BOM.md`: rewritten from §3.3, with Basic/Extended and lifecycle
  columns and the ordering cost note.
- `docs/kicad-workflow.md`: v2.3 state, the paste/stencil rule, the
  module footprint change, the CPL rotation table.
- `CLAUDE.md` hard rules: "Never use TXB0108/TXS0108E/74FST3257" stays;
  add "U10 is a 74LVC245A at 3.3 V, Pico on the A side"; replace the
  U15 1G11 quirk with the 74LVC00 NAND-NAND; net naming rule adds
  `_DBG` to the banned list; JP2 default.
- `docs/firmware-architecture.md`: one table for the Plus-W pin plan
  (header 31/32/34 = GP40/41/42; pad grid per §3.2) marked "not yet
  implemented in firmware".
- `docs/RP2350B_IDEAS.md` §13.5: open items closed by this spec.

## 8. Verification, in order

1. `python3 tools/gen_schematic.py` runs its self-check clean;
   `gen_breakout.py` regenerates the breakout with no diff.
2. ERC via `kicad-cli sch erc`: exactly the one known COCO-CART
   pin_to_pin error, zero warnings.
3. Update PCB from Schematic in the GUI with "delete footprints with no
   symbol"; `tools/place_pcb.py`; route; zones.
4. `kicad-cli pcb drc --schematic-parity` with the sandbox disabled:
   zero errors, zero unconnected.
5. `kicad-cli pcb render` top and bottom images checked by eye: module
   orientation, USB reachable, antenna rule area, nothing under the
   module, JP2 bridged 1-2, fiducials.
6. `tools/gen_fab.sh`; open the Gerbers in JLCPCB's viewer; upload BOM
   and CPL; fix rotations; record the table.
7. Order: 5 PCBs, 2 assembled (or 5), options per §6.5. Order number
   and date appended to READ-BEFORE-ORDERING.

## 9. Open items

- U15 LCSC number and the Nexperia SOIC-20 price: confirm at order.
- CPL rotation offsets: verified once in the JLCPCB preview, then fixed
  in the script.
- C12 exact part: pick at order from stock; footprint is D8 x 10 mm.
- Whether the JLCPCB order form accepts gold fingers together with
  economic assembly in one order (it did for the breakout bare boards;
  assembly was not used then). If not, fall back to standard assembly
  for this order and note the cost.

## 10. Future HDMI variant (not built in this spin)

HSTX, the RP2350's DVI output, exists only on GP12..GP19, which are the
header pins carrying A4..A11 in this pin plan (RP2350B_IDEAS §1, §4).
An HDMI build is therefore a Plus-W-only re-route of this carrier:
A4..A11 move to the pad grid, which then carries A4..A11, A14, A15,
CTS_BUF, SCS_BUF, Q_BUF, SLENB_BUF and OE_FW (all 15 pads; the /NMI and
/CART drive provisions are dropped in that variant), and GP12..GP19
route to a full-size HDMI-A receptacle with 8 series resistors and ESD
in the reserved top-right corner (§5). What v2.3 fixes now so that
variant is a routing change only: the module orientation, the reserved
corner, and the buffer placement. The generator gets a second pin table
for it when it is built. Sound over HDMI and the VDG snoop renderer are
firmware on top of that (RP2350B_IDEAS §4).


## 11. Addendum 2026-09-18: outline growth and pre-order fixes (v2.3.1)

Measured on the bench CoCo 3 (2026-09-18): cartridge opening 4 1/2 x 1 3/16 in
(114.3 x 30.2 mm, sized for a cased Program Pak), connector face 1 11/16 in
(43 mm, earlier note 44.5 mm) inside the case. Measured with the breakout on 2026-09-18: the connector
swallows only ~6 mm of tongue (its slot depth), so the shoulders 10.16 mm
from the tip never reach the connector face and the case surface sits
~49 mm from the finger tip. The 98 mm body
therefore passes the opening (the breakout README's "no wider than the finger
tab" note was wrong and is corrected), but the module's USB-C port at 53 mm
from the tip lands on the case surface and the HDMI reserve only 10 mm outside
it. Decision: grow the body 12 mm away from the fingers.

- Outline: 98 x 67.0 mm body + 10.16 mm tongue (77.16 mm overall); board y now
  32.187..109.347. Everything at or above JP2's row (module U1, JP3, C10, TP1-6,
  antenna keepout, HDMI reserve, top-edge silk) moves up 12 mm; the buffer row,
  power block and sound stage stay. USB-C ends up ~16 mm outside the case.
- JP3 default flips to the audio position: pin 1 = AUDIO_PWM, pin 3 = E_BUF
  (bridged 1-2 footprint kept), so a Pico 2 build has sound without cutting a
  jumper. Plus-W audio still arrives on pad GP34; do not bridge 2-3 and drive
  GP34 at the same time.
- Schematic fixes from the pre-order reviews: R7 100 k -> 10 k (RP2350 reset
  pull-down beat 100 k, the /HALT boot hold was a lottery); R10 DNP (it held RUN
  low when the board was USB-only); R3 populated (no floating /RESET buffer
  input on the bench); R23 10 k pull-up on SLENB_CART (pin 40 is cart->CoCo,
  nothing drives it); C15 moved in series ahead of R21 as a 1 uF DNP with R24
  0 R bypass populated (the old parallel C15 was a treble boost, not AC
  coupling); JP4 2-pad solder jumper Q_CART -> CART_CART (open; autostart tie
  for Pico 2 builds); R25 10 k pull-up on U10_OE (an unprogrammed module with
  JP2 in the firmware position drove the CoCo bus); TP8 = GND; FID3 third
  fiducial; 100 k LCSC C17407 is discontinued -> C149504; 0 R = C17477.
- Layout fixes: cart GND and +5V neck tracks widened to 0.5 mm (GND also on B.Cu with a via); sound cluster
  moved 1 mm off the right edge; "THIS SIDE UP" silk; order remark naming the
  tongue edge for the 30 deg bevel; the module stencil is used for the 15 grid
  pads only (castellation apertures sit over holes).
- Known and accepted: no mounting holes unless the new strip has room; silk
  lines at 0.12 mm (KiCad library default) and JP legends at 0.8 mm; C12 is a
  10 mm can if populated; a Program Pak shell needs the roadmap board.
