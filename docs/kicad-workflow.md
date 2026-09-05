# KiCad Workflow — PiCoCo v2.1 Redesign

What's in the repo right now, what you do with it in KiCad, and how
to verify the redesign is correct at each step.

Current schematic revision: **v2.2** (combined OE_BUS, firmware /HALT
drive, RUN from /RESET, new Schottky‑fed VSYS, 3‑input AND for /OE,
debug 2×20 headers removed to save board area).

---

## 1. Current state of the files

| File | State |
|------|-------|
| `PiCoCo/PiCoCo.kicad_sch` | **v2.2 schematic** — generated from `tools/gen_schematic.py`. Adds Q2 (2N7002), D2 (SS14), R6–R14, JP1, C11, TP1–TP6; swaps U15 from 74LVC1G08 to 74LVC1G11 (3‑input); renames CTS_B/SCS_B/RW_B to OE_BUS/HALT_GATE/RW_BUF; removes J_CART and J_LVC 2×20 debug breakouts. ERC‑clean except one known cosmetic warning (COCO‑CART symbol library quirk). |
| `PiCoCo/PiCoCo.kicad_sch.v1-backup` | **Old schematic** — preserved for reference. |
| `PiCoCo/PiCoCo.kicad_pcb` | **Needs Update‑from‑Schematic** — reflects an earlier revision until you run the sync step below. |
| `PiCoCo/PiCoCo.kicad_pcb.v1-backup` | PCB backup (original v1). |
| `PiCoCo/PiCoCo.kicad_pcb.pre-place` | Backup produced by `tools/place_pcb.py`. |
| `PiCoCo/PiCoCo.kicad_pro` | Design rules: 0.2 mm signal / 0.5 mm power net class; 0.15 mm clearance; 0.3 mm edge clearance; 0.6 mm min via. |
| `PiCoCo/fp-lib-table` and `sym-lib-table` | Use `${KIPRJMOD}/../libraries/` relative path. |
| `libraries/PiCoCo.pretty/COCO-CART-2.1X1.75.kicad_mod` | Unchanged — edge connector footprint. |
| `libraries/PiCoCo.kicad_sym` | Local symbols: `Pico`, `COCO-CART`. Other new symbols (74LVC1G11, 2N7002, Schottky, jumpers, TestPoint) come from stock KiCad libraries — no changes needed here. |
| `tools/gen_schematic.py` | Generator script. `python3 tools/gen_schematic.py` regenerates the schematic from the declarative placement + pin‑map. |
| `tools/place_pcb.py` | Coarse footprint placement. Edits the first `(at X Y rot)` of each footprint. Idempotent. |
| `tools/gen_fab.sh` | Wraps `kicad-cli` to produce Gerbers, drill, and pick‑and‑place in `fab/`. |
| `docs/hardware-design.md` | Full hardware design reference. |
| `docs/firmware-architecture.md` | Firmware architecture and bring‑up plan. |
| `fab/` | Fab outputs (Gerbers, drill, position) + `READ-BEFORE-ORDERING.txt`. |

## 2. What to do in KiCad (step by step)

### 2.1 Open the project

1. Launch KiCad.
2. `File → Open Project…` → select `PiCoCo/PiCoCo.kicad_pro`.
3. Symbol and footprint libraries should resolve automatically thanks
   to the fixed `fp-lib-table` / `sym-lib-table` (they now use
   `${KIPRJMOD}`).

### 2.2 Review the new schematic

1. Open Eeschema (`File → Open Schematic` or the schematic icon).
2. You should see all the v2.0 components placed in a grid. **It
   won't be pretty** — the generator places components on a grid
   with pins connected via global labels touching pin endpoints.
   Global labels are the wiring.
3. Run ERC: `Inspect → Electrical Rules Checker`. Expect **1 error**
   and 0 warnings. The error is the cosmetic `pin_to_pin` between
   P1 Pin 33 GND@1 and P1 Pin 34 GND@2 (both declared `power_output`
   in the `PiCoCo:COCO-CART` symbol — a library-level quirk). Ignore.
4. Optional — reshape the schematic for readability:
   - Rotate/move components so wires are visually obvious.
   - Convert clusters of global labels into net labels + short wires
     for nearby pins.
   - None of this affects the netlist; it's only for human reading.

### 2.3 Sync the PCB

1. Close the schematic editor first (KiCad holds file locks).
2. From the PCB editor: `Tools → Update PCB from Schematic…`
3. **Tick "Delete footprints with no corresponding symbol"** so old
   orphans get cleaned up automatically.
4. KiCad will show a diff. For the v2.0 → v2.1 step, expect these
   changes:
   - **U15** footprint change (SC‑70‑5 → SC‑70‑6); KiCad removes
     the old footprint and adds the new one at origin.
   - **Added**: Q2 (SOT‑23), D2 (SMA), R6–R14 (0805), JP1 (1×2
     header), C11 (0805), TP1–TP6 (1×1 mm SMD pads).
   - **Net renames**: nets previously named `CTS_B`, `SCS_B`, `RW_B`
     are now `OE_BUS`, `HALT_GATE`, `RW_BUF` (plus `_RAW` variants on
     the pre‑termination side). Any hand‑routed traces on those nets
     will be preserved under their new names as long as the endpoints
     still match.
5. Confirm. New footprints will land at origin; run
   `python3 tools/place_pcb.py` to move them to their planned
   positions.

### 2.4 Place components

Suggested physical layout (looking at the board with the edge fingers
at the bottom):

```
   +------------------------------------------+
   |  [J_LVC 2x20]         [J_CART 2x20]      |  <- debug headers
   |                                           |
   |   [U1 Pi Pico 2]                          |
   |                         [R1][R2][R3]      |  <- pull-ups
   |                         [R4]              |
   |                                           |
   |   [U11][U12][U13]        [J_SWD]          |  <- address/control buffers
   |    C6   C7   C8                           |
   |                                           |
   |            [U10]           [U15]          |  <- data bus + OE gate
   |             C4 C5           C9            |
   |                                           |
   |            [U14 LDO]  C2 C3               |  <- power
   |            [C1 bulk]                      |
   |                                           |
   +--[P1 COCO-CART edge fingers, 40 fingers]--+
```

Drag each footprint into place. Keep decoupling caps within 5 mm of
the IC they serve.

### 2.5 Route

- Netclass `Power` (nets `+5V`, `+3V3`, `GND`): 0.5 mm tracks.
- Netclass `Default` (everything else): 0.2 mm.
- Use `Route → Route Tracks` (X). Prefer short, direct runs.

### 2.6 Add ground zones (the critical missing piece from v1)

1. Select the `B.Cu` layer.
2. `Place → Add Filled Zone` (or keyboard Ctrl+Shift+Z).
3. Click-draw a rectangle that covers the entire board area.
4. In the Zone Properties dialog:
   - Net: **GND**
   - Layer: B.Cu
   - Clearance: 0.2 mm
   - Minimum width: 0.2 mm
   - Thermal relief: default
   - Zone priority: 0
5. Hit OK. KiCad fills the back side with GND copper.
6. Repeat on `F.Cu` — also assign to GND net. The front pour will
   route around component pads but fill all unused copper with ground.
7. Add stitching vias: around the board perimeter, under each IC,
   and along power traces. Place with `V` shortcut then drop on any
   unused area; KiCad defaults it to GND if it's in the GND zone.
8. Refill zones: `Edit → Fill All Zones` (`B` shortcut).

### 2.7 DRC

1. `Inspect → Design Rule Checker` (Ctrl+Shift+F7 in the PCB editor).
2. Fix each error. Expect nothing severe — unrouted tracks if you
   haven't finished routing.

### 2.8 Edge fingers / gold

- Verify the edge connector fingers on P1 look like 40 gold-plated
  pads on both `F.Cu` and `B.Cu`, right at the board edge.
- When ordering from JLCPCB:
  - Select **Gold Fingers: Yes**
  - Bevel angle: **30°**
  - Gold thickness: **1 µm** minimum
  - Surface finish for rest of board: **HASL** or **ENIG** (your pick)

## 3. Regenerating the schematic

If you want to change pin assignments or add components, edit
`tools/gen_schematic.py` (declarative — modify the `pin_nets`
dictionaries) and re‑run:

```bash
python3 tools/gen_schematic.py
```

Then:
1. **Close Eeschema first** (file‑lock races otherwise).
2. `python3 tools/gen_schematic.py` — overwrites the .kicad_sch.
3. Reopen Eeschema, run ERC. Expect 1 ignored warning (see §2.2).
4. PCB editor: `Tools → Update PCB from Schematic`, **with
   "Delete footprints with no corresponding symbol" ticked**. This
   removes orphans from net renames.
5. `python3 tools/place_pcb.py` — move any newly added footprints
   to their planned positions (idempotent; it's safe to run again
   after hand‑tweaks).
6. Refill zones (`B` in pcbnew), run DRC, address any issues.

> Note: `kicad-cli pcb drc` is broken on current macOS (Swift
> runtime error). Use the KiCad GUI for DRC.

## 3.1 Generating fab outputs

```bash
bash tools/gen_fab.sh
```

Produces Gerbers, drill file, and position file under `fab/`. The
shipped `fab/READ-BEFORE-ORDERING.txt` is a checklist for JLCPCB:
hard gold fingers, 30° bevel, 1 µm gold, 1.6 mm board. **Do not
skip** — JLCPCB's defaults (HASL on fingers) will destroy a CoCo
slot over time.

## 4. Common gotchas

- **Symbol library errors on open.** KiCad may complain that
  `PiCoCo:Pico` or `PiCoCo:COCO-CART` is not in the global lib.
  That's fine — the symbols are embedded in the schematic's
  `lib_symbols` block, and the project-scoped `sym-lib-table`
  points at `${KIPRJMOD}/../libraries/`. If KiCad still complains,
  check `Preferences → Manage Symbol Libraries → Project Specific
  Libraries` shows a `PiCoCo` entry.
- **Footprint not found on "Update PCB from Schematic".** All
  footprints use standard KiCad libraries except the edge
  connector (`PiCoCo:COCO-CART-2.1X1.75`) and the Pico
  (`PiCoCo:RPi_Pico_SMD_TH`). Both come from the project's local
  `fp-lib-table`. If KiCad complains about any standard one
  (e.g. `Package_SO:SOIC-24W_7.5x15.4mm_P1.27mm`), check
  `Preferences → Manage Footprint Libraries → Global Libraries`
  and make sure the standard KiCad libraries are enabled.
- **ERC warnings about `tri_state` pins on U15 inputs.** The
  74LS245 symbol declares its B-side pins as `tri_state` which
  doesn't satisfy KiCad's "Input needs Output to drive it" rule
  when those pins feed the AND gate (U15). These are cosmetic
  and can be suppressed in `Schematic Setup → Electrical Rules`
  or left as warnings.

## 5. Bring-up plan

See `docs/firmware-architecture.md` §9 — the firmware bring-up plan.
For the hardware alone, steps 1–2 (board fab visual + power) are the
first sanity checks before flashing the Pico.
