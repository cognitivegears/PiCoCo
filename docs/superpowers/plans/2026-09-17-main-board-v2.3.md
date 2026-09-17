# Main Board v2.3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the approved v2.3 spec into a regenerated schematic, a carrier footprint, a placed and routed PCB, and a JLCPCB order package, ending with boards on order.

**Architecture:** `tools/gen_schematic.py` stays the single source of the schematic and now also writes the two local library items it depends on (the carrier symbol/footprint and a single-unit 74LVC00 symbol). `tools/place_pcb.py` does coarse placement plus the rule areas and silkscreen the spec requires; routing is done by the user in the KiCad GUI with DRC run from the command line. `tools/gen_fab.sh` grows the JLCPCB BOM/CPL and the module-stencil variant.

**Tech Stack:** Python 3.12 (stdlib only for the generators; KiCad's bundled Python + `pcbnew` for the breakout regression), KiCad 10.0.6 `kicad-cli`, git.

**Spec:** `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`

## Global Constraints

- Never hand-edit `PiCoCo/PiCoCo.kicad_sch`; regenerate it (CLAUDE.md).
- Packages: SOIC-20W, SOIC-14, SOT-23, SOT-223, SMA, 0805, D8 electrolytic only. No TSSOP, no SC-70.
- Net naming: buffered signals end in `_BUF`; `_RAW` only on the pre-termination side of R11/R12; `_CART` on cart-side nets. `_B` and `_DBG` are banned.
- U10 is a 74LVC245A at 3.3 V: Pico D0..D7 on pins 2..9 (A side), cart D0..D7 on pins 18..11 (B side), DIR = `RW_BUF`, /OE = `U10_OE`.
- Decode: `OE_BUS_RAW = NAND(NAND(CTS_BUF, SCS_BUF), E_BUF)`.
- Module pads carry no F.Paste in the production Gerbers; nothing on the top side under the module.
- Board outline unchanged: 98 x 55 mm, `P1` at (121.92, 109.347), board x 99.59..197.59, y 44.187..99.187 (mm, KiCad Y down).
- ERC target: exactly one error (COCO-CART `pin_to_pin` GND@1/GND@2), zero warnings. DRC target: zero errors, zero unconnected, `--schematic-parity` clean. `kicad-cli pcb drc` must run with the sandbox disabled.
- Commit after every task; never commit `PiCoCo/*.v1-backup`, `*.pre-place`, or files outside the task's list.
- `kicad-cli` path: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`. KiCad's Python for pcbnew: `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3`.

---

### Task 1: Library items written by the generator

**Files:**
- Modify: `tools/gen_schematic.py` (add three writer functions and call them from `main()`)
- Modify: `libraries/PiCoCo.pretty/COCO-CART-2.1X1.75.kicad_mod` (finger trim, in place, by the script)
- Create (by the script): `libraries/PiCoCo.pretty/Pico-Carrier.kicad_mod`
- Modify (by the script): `libraries/PiCoCo.kicad_sym` (adds `Pico-Carrier` and `74LVC00` symbols; existing symbols untouched)
- Test: `tools/check_library.py` (new, stdlib, assert-based)

**Interfaces:**
- Produces: footprint `PiCoCo:Pico-Carrier` with pads `1`..`43` as in `RPi_Pico_SMD_TH` plus `GP24`,`GP25`,`GP26`,`GP27`,`GP28`,`GP29`,`GP30`,`GP31`,`GP32`,`GP33`,`GP34`,`GP35`,`GP43`,`GP44`,`GP45`; symbol `PiCoCo:Pico-Carrier` with the same pin numbers; symbol `PiCoCo:74LVC00` pins `1`..`14` (1A 1B 1Y 2A 2B 2Y GND 3Y 3A 3B 4Y 4A 4B VCC), single unit, footprint `Package_SO:SOIC-14_3.9x8.7mm_P1.27mm`.
- Consumes: `g.extract_symbol`, `g._extract_pin_blocks` already in `gen_schematic.py`; the regex used by `gen_breakout.write_fingers_footprint`.

- [ ] **Step 1: Write the failing check**

Create `tools/check_library.py`:

```python
#!/usr/bin/env python3
"""Asserts the v2.3 library items exist and are geometrically right. Stdlib only."""
import re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
PRETTY = ROOT / "libraries" / "PiCoCo.pretty"
SYMS = ROOT / "libraries" / "PiCoCo.kicad_sym"

GRID = {  # pad number -> (x, y) in the footprint frame, RP2350B_IDEAS §13.4
    "GP26": (-5.08, 22.40), "GP29": (-2.54, 22.40), "GP32": (0.0, 22.40), "GP35": (2.54, 22.40), "GP45": (5.08, 22.40),
    "GP25": (-5.08, 19.86), "GP28": (-2.54, 19.86), "GP31": (0.0, 19.86), "GP34": (2.54, 19.86), "GP44": (5.08, 19.86),
    "GP24": (-5.08, 17.32), "GP27": (-2.54, 17.32), "GP30": (0.0, 17.32), "GP33": (2.54, 17.32), "GP43": (5.08, 17.32),
}

def pads(text):
    out = {}
    for m in re.finditer(r'\(pad "([^"]+)" (\w+) \w+ \(at ([-\d.]+) ([-\d.]+)[^)]*\) \(size ([\d.]+) ([\d.]+)\)[^\n]*?\(layers ([^)]*)\)', text):
        out.setdefault(m.group(1), []).append((m.group(2), float(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6)), m.group(7)))
    return out

def main():
    fp = (PRETTY / "Pico-Carrier.kicad_mod").read_text()
    assert fp.startswith('(footprint "Pico-Carrier"'), "footprint name"
    p = pads(fp)
    for num, (x, y) in GRID.items():
        assert num in p, f"missing pad {num}"
        kind, px, py, w, h, layers = p[num][0]
        assert kind == "smd" and abs(px - x) < 0.005 and abs(py - y) < 0.005, f"{num} at {px},{py}"
        assert abs(w - 1.8) < 0.005 and abs(h - 1.8) < 0.005, f"{num} size"
        assert "F.Paste" not in layers and "F.Cu" in layers, f"{num} layers {layers}"
    for num in ("1", "20", "21", "40"):
        assert num in p, f"missing header pad {num}"
        for kind, px, py, w, h, layers in p[num]:
            assert "Paste" not in layers, f"header pad {num} has paste"
    assert "antenna" in fp.lower(), "antenna keepout text missing"
    assert re.search(r'\(fp_rect \(start -10\.5 -25\.5\) \(end 10\.5 30\.42\)', fp), "courtyard extent"

    cart = (PRETTY / "COCO-CART-2.1X1.75.kicad_mod").read_text()
    assert "-5.207) (size 1.27 9.525)" not in cart, "fingers still end 0.44 mm from the edge"
    assert cart.count("-5.635) (size 1.27 8.67)") == 39, "39 trimmed fingers expected (pin 9 is short already)"

    sym = SYMS.read_text()
    assert '(symbol "Pico-Carrier"' in sym and '(symbol "74LVC00"' in sym
    car = sym[sym.index('(symbol "Pico-Carrier"'):]
    car = car[:car.index('\n  (symbol "', 10)] if '\n  (symbol "' in car[10:] else car
    for num in GRID:
        assert f'(number "{num}"' in car, f"symbol pin {num}"
    assert '(name "GP26/GP40"' in car and '(name "GP27/GP41"' in car and '(name "GP28/GP42"' in car
    nand = sym[sym.index('(symbol "74LVC00"'):]
    for n in range(1, 15):
        assert f'(number "{n}"' in nand, f"74LVC00 pin {n}"
    assert nand.count("(pin ") >= 14
    print("library ok")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/check_library.py`
Expected: `FileNotFoundError` for `Pico-Carrier.kicad_mod`.

- [ ] **Step 3: Add the writers to `tools/gen_schematic.py`**

Insert after `load_symbols()` (before the `world_pin_pos` helper):

```python
# ---------------------------------------------------------------------------
# Library items the v2.3 schematic depends on (written idempotently)
# ---------------------------------------------------------------------------
PRETTY = PROJECT_ROOT / "libraries" / "PiCoCo.pretty"

PAD_GRID = {  # RP2350B-Plus-W underside pads, footprint frame (RP2350B_IDEAS §13.4)
    "GP26": (-5.08, 22.40), "GP29": (-2.54, 22.40), "GP32": (0.0, 22.40), "GP35": (2.54, 22.40), "GP45": (5.08, 22.40),
    "GP25": (-5.08, 19.86), "GP28": (-2.54, 19.86), "GP31": (0.0, 19.86), "GP34": (2.54, 19.86), "GP44": (5.08, 19.86),
    "GP24": (-5.08, 17.32), "GP27": (-2.54, 17.32), "GP30": (0.0, 17.32), "GP33": (2.54, 17.32), "GP43": (5.08, 17.32),
}


def write_carrier_footprint() -> None:
    """Pico-Carrier = RPi_Pico_SMD_TH + the 15 Plus-W pads + antenna courtyard. No paste anywhere."""
    src = (PRETTY / "RPi_Pico_SMD_TH.kicad_mod").read_text()
    assert src.startswith('(footprint "RPi_Pico_SMD_TH"')
    text = src.replace('(footprint "RPi_Pico_SMD_TH"', '(footprint "Pico-Carrier"', 1)
    assert "F.Paste" not in text, "source footprint unexpectedly has paste"
    extra = []
    for num, (x, y) in PAD_GRID.items():
        extra.append(f'  (pad "{num}" smd rect (at {x} {y}) (size 1.8 1.8) (layers "F.Cu" "F.Mask") (tstamp {u()}))')
    # Plus-W envelope: 51 mm body (+-25.5) plus 4.92 mm antenna past the pin 20/21 end (+Y).
    extra.append('  (fp_rect (start -10.5 -25.5) (end 10.5 30.42) (stroke (width 0.05) (type default)) (fill none) (layer "F.CrtYd") (tstamp %s))' % u())
    extra.append('  (fp_rect (start -10.5 25.5) (end 10.5 30.42) (stroke (width 0.1) (type default)) (fill none) (layer "F.Fab") (tstamp %s))' % u())
    extra.append('  (fp_text user "ANT antenna keepout (Plus-W)" (at 0 28) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))) (tstamp %s))' % u())
    extra.append('  (fp_text user "USB" (at 0 -23) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))) (tstamp %s))' % u())
    extra.append('  (fp_text user "ANT" (at 0 27.5) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))) (tstamp %s))' % u())
    end = text.rstrip().rfind(")")
    text = text[:end].rstrip("\n") + "\n" + "\n".join(extra) + "\n)\n"
    (PRETTY / "Pico-Carrier.kicad_mod").write_text(text)


def trim_cart_fingers() -> None:
    """Fingers end 1.30 mm from the edge (JLCPCB 30 deg bevel is 1.13 mm deep). Idempotent."""
    p = PRETTY / "COCO-CART-2.1X1.75.kicad_mod"
    src = p.read_text()
    out = re.sub(r"\(at ([\d.]+) -5\.207\) \(size 1\.27 9\.525\)", r"(at \1 -5.635) (size 1.27 8.67)", src)
    if out != src:
        p.write_text(out)


def _replace_or_append_symbol(name: str, sym: str) -> None:
    lib = LOCAL_SYMS
    text = lib.read_text()
    try:
        old = extract_symbol(lib, name)
        text = text.replace(old, sym.strip("\n"))
    except RuntimeError:
        end = text.rstrip().rfind(")")
        text = text[:end].rstrip("\n") + "\n" + sym.rstrip("\n") + "\n" + text[end:]
    lib.write_text(text)


def write_carrier_symbol() -> None:
    """Pico-Carrier symbol = Pico symbol widened, side pins pushed out, 15 pad pins along the top."""
    src = extract_symbol(LOCAL_SYMS, "Pico")
    sym = re.sub(r'"Pico(_\d+_\d+)?"', r'"Pico-Carrier\1"', src)
    sym = sym.replace("(at -17.78 ", "(at -21.59 ").replace("(at 17.78 ", "(at 21.59 ")
    sym = sym.replace("(rectangle (start -15.24 26.67) (end 15.24 -26.67)", "(rectangle (start -19.05 26.67) (end 19.05 -26.67)")
    for old, new in (("GP26", "GP26/GP40"), ("GP27", "GP27/GP41"), ("GP28", "GP28/GP42")):
        assert f'(name "{old}"' in sym, f"pin name {old} not found in Pico symbol"
        sym = sym.replace(f'(name "{old}"', f'(name "{new}"', 1)
    pins = []
    for i, num in enumerate(sorted(PAD_GRID, key=lambda n: int(n[2:]))):
        x = -17.78 + 2.54 * i
        pins.append(
            f'      (pin bidirectional line (at {x:.2f} 29.21 270) (length 2.54)\n'
            f'        (name "{num}" (effects (font (size 1.27 1.27))))\n'
            f'        (number "{num}" (effects (font (size 1.27 1.27))))\n'
            f'      )'
        )
    # append the pins to the last pin-bearing sub-symbol block
    blocks = _extract_pin_blocks(sym)
    last = blocks[-1]
    sym = sym.replace(last, last + "\n" + "\n".join(pins), 1)
    sym = re.sub(r'\(property "Footprint" "[^"]*"', '(property "Footprint" "PiCoCo:Pico-Carrier"', sym)
    _replace_or_append_symbol("Pico-Carrier", sym)


def write_lvc00_symbol() -> None:
    """Single-unit quad NAND (the stock 74LS00 is 5 units; the generator places one unit per symbol)."""
    left = [("1", "1A", 7.62), ("2", "1B", 5.08), ("4", "2A", 2.54), ("5", "2B", 0.0),
            ("9", "3A", -2.54), ("10", "3B", -5.08), ("12", "4A", -7.62), ("13", "4B", -10.16)]
    right = [("3", "1Y", 6.35), ("6", "2Y", 1.27), ("8", "3Y", -3.81), ("11", "4Y", -8.89)]
    pins = []
    for num, name, y in left:
        pins.append(f'      (pin input line (at -12.7 {y} 0) (length 2.54)\n        (name "{name}" (effects (font (size 1.27 1.27))))\n        (number "{num}" (effects (font (size 1.27 1.27))))\n      )')
    for num, name, y in right:
        pins.append(f'      (pin output line (at 12.7 {y} 180) (length 2.54)\n        (name "{name}" (effects (font (size 1.27 1.27))))\n        (number "{num}" (effects (font (size 1.27 1.27))))\n      )')
    pins.append('      (pin power_in line (at 0 12.7 270) (length 2.54)\n        (name "VCC" (effects (font (size 1.27 1.27))))\n        (number "14" (effects (font (size 1.27 1.27))))\n      )')
    pins.append('      (pin power_in line (at 0 -15.24 90) (length 2.54)\n        (name "GND" (effects (font (size 1.27 1.27))))\n        (number "7" (effects (font (size 1.27 1.27))))\n      )')
    sym = (
        '  (symbol "74LVC00" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)\n'
        '    (property "Reference" "U" (at 0 15.24 0) (effects (font (size 1.27 1.27))))\n'
        '    (property "Value" "74LVC00" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))\n'
        '    (property "Footprint" "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
        '    (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
        '    (symbol "74LVC00_0_1"\n'
        '      (rectangle (start -10.16 10.16) (end 10.16 -12.7) (stroke (width 0.254) (type default)) (fill (type background)))\n'
        '    )\n'
        '    (symbol "74LVC00_1_1"\n' + "\n".join(pins) + '\n    )\n'
        '  )'
    )
    _replace_or_append_symbol("74LVC00", sym)


def write_library_items() -> None:
    write_carrier_footprint()
    trim_cart_fingers()
    write_carrier_symbol()
    write_lvc00_symbol()
```

Then change `main()`:

```python
def main() -> None:
    write_library_items()
    text = build()
    SCH_OUT.write_text(text)
    print(f"wrote {SCH_OUT}  ({len(text):,} bytes)")
```

Note `build()` still references the old symbols at this point; run only the library step for now:

Run: `python3 -c "import sys; sys.path.insert(0,'tools'); import gen_schematic as g; g.write_library_items()"`

- [ ] **Step 4: Run the check**

Run: `python3 tools/check_library.py`
Expected: `library ok`. If `_extract_pin_blocks` returns blocks whose last element is not inside the pin sub-symbol, print `blocks[-1][:80]` and adjust the insertion to the block that contains `(number "43"`.

- [ ] **Step 5: Open the symbol and footprint once in KiCad** (user): Symbol Editor → PiCoCo → Pico-Carrier shows 15 pins along the top, wider body; 74LVC00 shows 14 pins. Footprint Editor → Pico-Carrier shows the 3x5 pads between the header rows near the ANT end. Fix any pin overlapping the body outline by adjusting the constants above, rerun steps 3-4.

- [ ] **Step 6: Commit**

```bash
git add tools/gen_schematic.py tools/check_library.py libraries/PiCoCo.kicad_sym libraries/PiCoCo.pretty/Pico-Carrier.kicad_mod libraries/PiCoCo.pretty/COCO-CART-2.1X1.75.kicad_mod
git commit -m "kicad: Pico-Carrier footprint/symbol (Plus-W pad grid), 74LVC00 symbol, finger trim to 1.30 mm"
```

---

### Task 2: Schematic v2.3 in the generator, with self-check and ERC

**Files:**
- Modify: `tools/gen_schematic.py` (`symbol_instance`, `Sheet.place`, `SYMBOLS`, `build()`, new `check()`, `main()`)
- Generated: `PiCoCo/PiCoCo.kicad_sch`
- Test: the generator's own `check()`; `kicad-cli sch erc`

**Interfaces:**
- Consumes: `PiCoCo:Pico-Carrier`, `PiCoCo:74LVC00` from Task 1.
- Produces: the v2.3 netlist (names exactly as in spec §3); symbol properties `LCSC`, `MPN`; DNP flag on R2, R3, C12, C15, Q3, Q4, R15, R16, R17, R18. Refs used by Task 4: U1, P1, U10..U15, JP2, JP3, Q2..Q4, D2, R1..R4, R7..R22, C1..C4, C6..C10, C12..C15, TP1..TP7, J_SWD, FID1, FID2.

- [ ] **Step 1: Extend `symbol_instance` and `Sheet.place` with `dnp`, `lcsc`, `mpn`, `in_bom`**

Replace the `symbol_instance` signature and the `(dnp no)` line, and add two properties:

```python
def symbol_instance(
    lib_id: str, ref: str, value: str,
    x: float, y: float, rot: int = 0, mirror: str = "",
    footprint: str = "", unit: int = 1,
    in_bom: str = "yes", on_board: str = "yes",
    dnp: bool = False, lcsc: str = "", mpn: str = "",
) -> str:
```

after the Datasheet property append:

```python
    props.append(
        f'    (property "LCSC" "{lcsc}" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )'
    )
    props.append(
        f'    (property "MPN" "{mpn}" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )'
    )
```

and use `(dnp {"yes" if dnp else "no"})` in the header line. In `Sheet.place` add the same three keyword parameters (`dnp: bool = False, lcsc: str = "", mpn: str = "", in_bom: str = "yes"`) and pass them through to `symbol_instance(...)`. Also record placements for the self-check: in `Sheet.__init__` add `self.placed: list[dict] = []`, and at the top of `place()`:

```python
        self.placed.append({"ref": ref, "lib_id": lib_id, "value": value, "pin_nets": dict(pin_nets or {}),
                            "dnp": dnp, "lcsc": lcsc, "in_bom": in_bom})
```

- [ ] **Step 2: Replace `SYMBOLS`**

```python
SYMBOLS = [
    ("PiCoCo:Pico-Carrier", LOCAL_SYMS, "Pico-Carrier"),
    ("PiCoCo:COCO-CART", LOCAL_SYMS, "COCO-CART"),
    ("PiCoCo:74LVC00", LOCAL_SYMS, "74LVC00"),
    # 74LS245 symbol is pin-compatible with the 74LVC245A; Value carries the real part.
    ("74xx:74LS245", KICAD_STOCK / "74xx.kicad_sym", "74LS245"),
    ("Regulator_Linear:AP1117-15", KICAD_STOCK / "Regulator_Linear.kicad_sym", "AP1117-15"),
    ("Transistor_FET:Q_NMOS_GSD", KICAD_STOCK / "Transistor_FET.kicad_sym", "Q_NMOS_GSD"),
    ("Device:R", KICAD_STOCK / "Device.kicad_sym", "R"),
    ("Device:C", KICAD_STOCK / "Device.kicad_sym", "C"),
    ("Device:C_Polarized", KICAD_STOCK / "Device.kicad_sym", "C_Polarized"),
    ("Device:D_Schottky", KICAD_STOCK / "Device.kicad_sym", "D_Schottky"),
    ("Jumper:SolderJumper_3_Bridged12", KICAD_STOCK / "Jumper.kicad_sym", "SolderJumper_3_Bridged12"),
    ("Mechanical:Fiducial", KICAD_STOCK / "Mechanical.kicad_sym", "Fiducial"),
    ("Connector_Generic:Conn_01x04", KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_01x04"),
    ("Connector:TestPoint", KICAD_STOCK / "Connector.kicad_sym", "TestPoint"),
    ("power:+5V", KICAD_STOCK / "power.kicad_sym", "+5V"),
    ("power:+3V3", KICAD_STOCK / "power.kicad_sym", "+3V3"),
    ("power:GND", KICAD_STOCK / "power.kicad_sym", "GND"),
    ("power:PWR_FLAG", KICAD_STOCK / "power.kicad_sym", "PWR_FLAG"),
]
```

Check each stock symbol exists before relying on it: `grep -c '(symbol "SolderJumper_3_Bridged12"' /Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/Jumper.kicad_sym` and the same for `Mechanical.kicad_sym` `Fiducial`, `Device.kicad_sym` `C_Polarized`. Pin numbers: SolderJumper_3_Bridged12 pins 1, 2 (centre), 3; Fiducial has no pins; C_Polarized pin 1 = +, pin 2 = −.

- [ ] **Step 3: Rewrite `build()`**

Replace the whole body from `# ---------- U1` to the `title_block` with the following. Constants: `LVC245 = ("74xx:74LS245", "SN74LVC245A", "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm", "C571201", "SN74LVC245ADWR")`, `R0805 = "Resistor_SMD:R_0805_2012Metric"`, `C0805 = "Capacitor_SMD:C_0805_2012Metric"`. LCSC numbers for resistors: 33R C17634, 100R C17408, 1k "C17513", 2.2k "C17520", 4.7k C17673, 10k C17414, 100k C17407; caps 10nF "C1710", 100nF C49678, 10uF C15850, 22uF C45783. (The four quoted ones were not in the 2026-09-17 lookup; confirm on lcsc.com when filling them in and correct if wrong.)

```python
    lib_block, pin_map = load_symbols()
    s = Sheet(pin_map)
    LVC245 = ("74xx:74LS245", "SN74LVC245A", "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm", "C571201", "SN74LVC245ADWR")
    R0805 = "Resistor_SMD:R_0805_2012Metric"
    C0805 = "Capacitor_SMD:C_0805_2012Metric"
    RLC = {"33": "C17634", "100": "C17408", "1k": "C17513", "2.2k": "C17520", "4.7k": "C17673", "10k": "C17414", "100k": "C17407"}
    CLC = {"10nF": "C1710", "100nF": "C49678", "10uF": "C15850", "22uF": "C45783"}

    def res(ref, value, x, y, a, b, dnp=False):
        s.place("Device:R", ref, value, x, y, pin_nets={"1": a, "2": b}, footprint=R0805,
                dnp=dnp, lcsc=RLC[value], mpn=f"0805 {value} 1%")

    def cap(ref, value, x, y, a, b, dnp=False):
        s.place("Device:C", ref, value, x, y, pin_nets={"1": a, "2": b}, footprint=C0805,
                dnp=dnp, lcsc=CLC[value], mpn=f"0805 {value}")

    # ---------- U1: module (Pico 2 or RP2350B-Plus-W) ----------
    pico_nets = {
        "1": "D0", "2": "D1", "3": "GND", "4": "D2", "5": "D3", "6": "D4", "7": "D5", "8": "GND",
        "9": "D6", "10": "D7", "11": "A0_BUF", "12": "A1_BUF", "13": "GND", "14": "A2_BUF",
        "15": "A3_BUF", "16": "A4_BUF", "17": "A5_BUF", "18": "GND", "19": "A6_BUF", "20": "A7_BUF",
        "21": "A8_BUF", "22": "A9_BUF", "23": "GND", "24": "A10_BUF", "25": "A11_BUF",
        "26": "A12_BUF", "27": "A13_BUF", "28": "GND", "29": "RW_BUF", "30": "PICO_RUN",
        "31": "OE_BUS", "32": "HALT_GATE", "33": "GND", "34": "PICO_P34",
        "37": "PICO_3V3_EN", "38": "GND", "39": "VSYS_PICO",
        "41": "SWCLK_PICO", "42": "GND", "43": "SWDIO_PICO",
        # Plus-W underside pads (NC on a Pico 2): spec §3.2
        "GP24": "CTS_BUF", "GP25": "SCS_BUF", "GP26": "E_BUF", "GP27": "Q_BUF", "GP28": "SLENB_BUF",
        "GP29": "A14_BUF", "GP30": "A15_BUF", "GP31": "OE_FW", "GP32": "NMI_DRV", "GP33": "CART_DRV",
        "GP34": "AUDIO_PWM",
    }
    s.place("PiCoCo:Pico-Carrier", "U1", "Pico 2 / RP2350B-Plus-W", 80.0, 120.0,
            pin_nets=pico_nets, footprint="PiCoCo:Pico-Carrier", in_bom="no")

    # ---------- P1: cartridge edge ----------
    cart_nets = {
        "3": "HALT_CART", "4": "NMI_CART", "5": "RESET_CART", "6": "E_CART", "7": "Q_CART",
        "8": "CART_CART", "9": "+5V",
        "10": "D0_CART", "11": "D1_CART", "12": "D2_CART", "13": "D3_CART",
        "14": "D4_CART", "15": "D5_CART", "16": "D6_CART", "17": "D7_CART",
        "18": "RW_CART",
        "19": "A0_CART", "20": "A1_CART", "21": "A2_CART", "22": "A3_CART",
        "23": "A4_CART", "24": "A5_CART", "25": "A6_CART", "26": "A7_CART",
        "27": "A8_CART", "28": "A9_CART", "29": "A10_CART", "30": "A11_CART", "31": "A12_CART",
        "32": "CTS_CART", "33": "GND", "34": "GND", "35": "SND_CART", "36": "SCS_CART",
        "37": "A13_CART", "38": "A14_CART", "39": "A15_CART", "40": "SLENB_CART",
    }
    s.place("PiCoCo:COCO-CART", "P1", "COCO-CART", 260.0, 140.0, pin_nets=cart_nets,
            footprint="PiCoCo:COCO-CART-2.1X1.75", in_bom="no")

    # ---------- U10: data buffer, Pico on A (2..9), cart on B (18..11) ----------
    u10 = {"1": "RW_BUF", "10": "GND", "19": "U10_OE", "20": "+3V3"}
    for i in range(8):
        u10[str(2 + i)] = f"D{i}"
        u10[str(18 - i)] = f"D{i}_CART"
    s.place(LVC245[0], "U10", LVC245[1], 170.0, 130.0, pin_nets=u10, footprint=LVC245[2], lcsc=LVC245[3], mpn=LVC245[4])

    # ---------- U11: A0..A7 ----------
    u11 = {"1": "+3V3", "10": "GND", "19": "GND", "20": "+3V3"}
    for i in range(8):
        u11[str(2 + i)] = f"A{i}_CART"
        u11[str(18 - i)] = f"A{i}_BUF"
    s.place(LVC245[0], "U11", LVC245[1], 80.0, 220.0, pin_nets=u11, footprint=LVC245[2], lcsc=LVC245[3], mpn=LVC245[4])

    # ---------- U12: A8..A13, R/W, /CTS ----------
    u12 = {"1": "+3V3", "10": "GND", "19": "GND", "20": "+3V3", "8": "RW_CART", "12": "RW_BUF_RAW",
           "9": "CTS_CART", "11": "CTS_BUF"}
    for i in range(6):
        u12[str(2 + i)] = f"A{8 + i}_CART"
        u12[str(18 - i)] = f"A{8 + i}_BUF"
    s.place(LVC245[0], "U12", LVC245[1], 130.0, 220.0, pin_nets=u12, footprint=LVC245[2], lcsc=LVC245[3], mpn=LVC245[4])

    # ---------- U13: /SCS, E, Q, /SLENB, /RESET, A14, A15 ----------
    u13 = {"1": "+3V3", "10": "GND", "19": "GND", "20": "+3V3",
           "2": "SCS_CART", "18": "SCS_BUF", "3": "E_CART", "17": "E_BUF",
           "4": "Q_CART", "16": "Q_BUF", "5": "SLENB_CART", "15": "SLENB_BUF",
           "6": "RESET_CART", "14": "RESET_BUF", "7": "A14_CART", "13": "A14_BUF",
           "8": "A15_CART", "12": "A15_BUF", "9": "GND"}          # pin 11 (B8) no-connect
    s.place(LVC245[0], "U13", LVC245[1], 180.0, 220.0, pin_nets=u13, footprint=LVC245[2], lcsc=LVC245[3], mpn=LVC245[4])

    # ---------- U14: LDO ----------
    s.place("Regulator_Linear:AP1117-15", "U14", "AMS1117-3.3", 260.0, 50.0,
            pin_nets={"1": "GND", "2": "+3V3", "3": "+5V"},
            footprint="Package_TO_SOT_SMD:SOT-223-3_TabPin2", lcsc="C6186", mpn="AMS1117-3.3")

    # ---------- U15: quad NAND, gates 1+2 = NAND-NAND decode ----------
    s.place("PiCoCo:74LVC00", "U15", "SN74LVC00A", 220.0, 80.0,
            pin_nets={"1": "CTS_BUF", "2": "SCS_BUF", "3": "SEL_N",
                      "4": "SEL_N", "5": "E_BUF", "6": "OE_BUS_RAW",
                      "9": "GND", "10": "GND", "12": "GND", "13": "GND",   # 8, 11 outputs NC
                      "7": "GND", "14": "+3V3"},
            footprint="Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", lcsc="", mpn="SN74LVC00AD")

    # ---------- JP2: U10 /OE source; JP3: header pin 34 = E or audio ----------
    SJ = "Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm"
    s.place("Jumper:SolderJumper_3_Bridged12", "JP2", "U10_OE_SEL", 200.0, 100.0,
            pin_nets={"1": "OE_BUS", "2": "U10_OE", "3": "OE_FW"}, footprint=SJ, in_bom="no")
    s.place("Jumper:SolderJumper_3_Bridged12", "JP3", "P34_SEL", 100.0, 60.0,
            pin_nets={"1": "E_BUF", "2": "PICO_P34", "3": "AUDIO_PWM"}, footprint=SJ, in_bom="no")

    # ---------- /HALT drive (Q2), /NMI and /CART drives (Q3, Q4: DNP) ----------
    res("R1", "4.7k", 30.0, 280.0, "+5V", "HALT_CART")
    res("R2", "4.7k", 40.0, 280.0, "+5V", "NMI_CART", dnp=True)
    res("R3", "4.7k", 50.0, 280.0, "+5V", "RESET_CART", dnp=True)
    res("R7", "100k", 40.0, 160.0, "+3V3", "GATE_Q2")
    res("R8", "100", 50.0, 160.0, "HALT_GATE", "GATE_Q2")
    s.place("Transistor_FET:Q_NMOS_GSD", "Q2", "2N7002", 60.0, 170.0,
            pin_nets={"1": "GATE_Q2", "2": "GND", "3": "HALT_CART"},
            footprint="Package_TO_SOT_SMD:SOT-23", lcsc="C8545", mpn="2N7002")
    for q, r_s, r_pd, drv, drain in (("Q3", "R15", "R17", "NMI_DRV", "NMI_CART"),
                                     ("Q4", "R16", "R18", "CART_DRV", "CART_CART")):
        gate = f"GATE_{q}"
        res(r_s, "100", 40.0 + 30 * (q == "Q4"), 190.0, drv, gate, dnp=True)
        res(r_pd, "100k", 50.0 + 30 * (q == "Q4"), 190.0, gate, "GND", dnp=True)
        s.place("Transistor_FET:Q_NMOS_GSD", q, "2N7002", 60.0 + 30 * (q == "Q4"), 200.0,
                pin_nets={"1": gate, "2": "GND", "3": drain},
                footprint="Package_TO_SOT_SMD:SOT-23", dnp=True, lcsc="C8545", mpn="2N7002")

    # ---------- Reset, 3V3_EN, series R, SWD ----------
    res("R4", "10k", 80.0, 280.0, "VSYS_PICO", "PICO_3V3_EN")
    res("R9", "100", 200.0, 260.0, "RESET_BUF", "PICO_RUN")
    res("R10", "10k", 210.0, 260.0, "+3V3", "PICO_RUN")
    res("R11", "33", 210.0, 90.0, "OE_BUS_RAW", "OE_BUS")
    res("R12", "33", 140.0, 230.0, "RW_BUF_RAW", "RW_BUF")
    res("R13", "100", 30.0, 225.0, "SWCLK_PICO", "SWCLK")
    res("R14", "100", 30.0, 235.0, "SWDIO_PICO", "SWDIO")

    # ---------- Sound stage: AUDIO_PWM -> 2-pole RC -> divider -> SND_CART ----------
    res("R19", "1k", 120.0, 300.0, "AUDIO_PWM", "AUDIO_F1")
    cap("C13", "10nF", 125.0, 310.0, "AUDIO_F1", "GND")
    res("R20", "1k", 130.0, 300.0, "AUDIO_F1", "AUDIO_F2")
    cap("C14", "10nF", 135.0, 310.0, "AUDIO_F2", "GND")
    res("R21", "2.2k", 140.0, 300.0, "AUDIO_F2", "SND_CART")
    res("R22", "1k", 150.0, 310.0, "SND_CART", "GND")
    cap("C15", "100nF", 145.0, 290.0, "AUDIO_F2", "SND_CART", dnp=True)   # AC-coupling option

    # ---------- Power ----------
    s.place("Device:D_Schottky", "D2", "SS14", 250.0, 70.0,
            pin_nets={"1": "VSYS_PICO", "2": "+5V"}, footprint="Diode_SMD:D_SMA", lcsc="C2480", mpn="SS14")
    cap("C1", "10uF", 300.0, 200.0, "+5V", "GND")
    cap("C2", "10uF", 240.0, 30.0, "+5V", "GND")
    cap("C3", "22uF", 280.0, 30.0, "+3V3", "GND")
    for ref, cx, cy in (("C4", 160.0, 100.0), ("C6", 90.0, 260.0), ("C7", 140.0, 260.0),
                        ("C8", 190.0, 260.0), ("C9", 230.0, 70.0), ("C10", 100.0, 100.0)):
        cap(ref, "100nF", cx, cy, "+3V3", "GND")
    s.place("Device:C_Polarized", "C12", "1000uF 6.3V", 270.0, 80.0,
            pin_nets={"1": "VSYS_PICO", "2": "GND"},
            footprint="Capacitor_SMD:CP_Elec_8x10", dnp=True, lcsc="", mpn="1000uF 6.3V SMD electrolytic D8x10")

    # ---------- Test points, SWD, fiducials ----------
    for ref, net, tx, ty in (("TP1", "OE_BUS", 320.0, 60.0), ("TP2", "RW_BUF", 320.0, 70.0),
                             ("TP3", "CTS_BUF", 320.0, 80.0), ("TP4", "SCS_BUF", 320.0, 90.0),
                             ("TP5", "E_BUF", 310.0, 60.0), ("TP6", "+3V3", 310.0, 70.0),
                             ("TP7", "SND_CART", 310.0, 80.0)):
        s.place("Connector:TestPoint", ref, net, tx, ty, pin_nets={"1": net},
                footprint="TestPoint:TestPoint_Pad_1.0x1.0mm", in_bom="no")
    s.place("Connector_Generic:Conn_01x04", "J_SWD", "SWD", 20.0, 230.0,
            pin_nets={"1": "SWCLK", "2": "GND", "3": "SWDIO", "4": "+3V3"},
            footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", in_bom="no")
    for ref, fx in (("FID1", 330.0), ("FID2", 340.0)):
        s.place("Mechanical:Fiducial", ref, "FID", fx, 30.0, footprint="Fiducial:Fiducial_1mm_Mask2mm", in_bom="no")

    # ---------- PWR_FLAG on VSYS_PICO (D2 cathode is passive; ERC needs a driver) ----------
    s.powers.append(power_port("power:PWR_FLAG", 250.0, 60.0, 0))
    s.labels.append(global_label("VSYS_PICO", 250.0, 60.0, 0, "left", "bidirectional"))
```

Title block: rev `"2.3"`, date `"2026-09-17"`, comment 1 `"4x LVC245A at 3.3 V (U10 data bidi, U11-U13 in), 74LVC00 NAND-NAND /OE = (CTS|SCS) & E"`, comment 2 `"GPIO: GP0-7=D0-D7, GP8-21=A0-A13, GP22=/R/W, hdr31=OE_BUS, hdr32=HALT_GATE, hdr34=E (JP3: audio); Plus-W pads GP24-30 capture, GP31 FW /OE, GP32/33 NMI/CART drive, GP34 audio"`, comment 3 unchanged, comment 4 `"Spec: docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md"`.

The `power_port("power:PWR_FLAG", ...)` call: `power_port` derives the value from the lib_id, so it emits value `PWR_FLAG`; the global label at the same point ties it to `VSYS_PICO`. If ERC still reports "Input Power pin not driven" on `+5V`/`GND`, add the same pair for that net.

- [ ] **Step 4: Add `check()` and call it from `main()`**

```python
def check(s: "Sheet") -> None:
    """ponytail: the two fab-blocking bugs of v2.2 and the naming rule, as asserts."""
    nets: dict[str, list[str]] = {}
    for c in s.placed:
        for pin, net in c["pin_nets"].items():
            nets.setdefault(net, []).append(f'{c["ref"]}.{pin}')
    for net, pins in nets.items():
        assert not net.endswith("_B") and "_DBG" not in net, f"banned net name {net}"
        if net in ("+5V", "+3V3", "GND", "VSYS_PICO"):
            continue
        assert len(pins) >= 2, f"net {net} has one pin: {pins}"
    u10 = next(c for c in s.placed if c["ref"] == "U10")["pin_nets"]
    assert [u10[str(2 + i)] for i in range(8)] == [f"D{i}" for i in range(8)], "U10 A side must be Pico D0..D7"
    assert [u10[str(18 - i)] for i in range(8)] == [f"D{i}_CART" for i in range(8)], "U10 B side must be cart D0..D7"
    u15 = next(c for c in s.placed if c["ref"] == "U15")["pin_nets"]
    assert (u15["1"], u15["2"], u15["3"]) == ("CTS_BUF", "SCS_BUF", "SEL_N") and (u15["4"], u15["5"], u15["6"]) == ("SEL_N", "E_BUF", "OE_BUS_RAW"), "decode wiring"
    jp3 = next(c for c in s.placed if c["ref"] == "JP3")["pin_nets"]
    assert jp3["1"] == "E_BUF" and jp3["2"] == "PICO_P34", "JP3 wiring"
    for c in s.placed:
        if c["in_bom"] == "yes" and not c["dnp"] and c["ref"] not in ("U15", "C12"):
            assert c["lcsc"], f'{c["ref"]} has no LCSC number'
    print(f"check ok: {len(s.placed)} symbols, {len(nets)} nets")
```

`build()` must return the sheet too: change the last line of `build()` to `return assemble(lib_block, s, "A2", title_block), s` and update `main()`:

```python
def main() -> None:
    write_library_items()
    text, sheet = build()
    check(sheet)
    SCH_OUT.write_text(text)
    print(f"wrote {SCH_OUT}  ({len(text):,} bytes)")
```

`gen_breakout.py` calls `g.load_symbols(...)`, `g.Sheet`, `g.assemble`; it does not call `build()`, so the return-type change does not affect it. Verify with `grep -n "g.build" tools/gen_breakout.py` (expect no output).

- [ ] **Step 5: Regenerate and run ERC**

Run: `python3 tools/gen_schematic.py`
Expected: `check ok: ... symbols, ... nets` and `wrote ...`.

Run:
```bash
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
$K sch erc --format json --severity-all --output /tmp/erc.json PiCoCo/PiCoCo.kicad_sch
python3 -c "
import json; d=json.load(open('/tmp/erc.json'))
v=[x for s in d['sheets'] for x in s['violations']]
err=[x for x in v if x['severity']=='error']; warn=[x for x in v if x['severity']=='warning']
print('errors',len(err),'warnings',len(warn))
for x in err+warn: print(x['severity'], x['type'], x['description'][:90])
"
```
Expected: `errors 1 warnings 0`, the error being `pin_to_pin` on P1 GND@1/GND@2. Fix anything else in `build()` (typical: a `power_in` pin with no driver → add a PWR_FLAG pair as in step 3; a label on a pin marked `output` colliding with another output → check the net table in spec §3.2).

- [ ] **Step 6: Commit**

```bash
git add tools/gen_schematic.py PiCoCo/PiCoCo.kicad_sch
git commit -m "kicad: schematic v2.3 (LVC245 data buffer, 74LVC00 NAND-NAND decode, Plus-W pads, JP2/JP3, sound stage, LCSC properties, self-check)"
```

---

### Task 3: Breakout regression

**Files:**
- Test only: `tools/gen_breakout.py` run under KiCad's Python; `breakout/`, `libraries/`

**Interfaces:**
- Consumes: `gen_schematic.symbol_instance` / `Sheet.place` signatures from Task 2 (new keyword arguments have defaults).

- [ ] **Step 1: Regenerate the breakout**

Run:
```bash
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tools/gen_breakout.py
```
Expected: completes without a traceback.

- [ ] **Step 2: Diff ignoring UUIDs**

```bash
for f in $(git diff --name-only breakout libraries); do
  echo "== $f"; git diff -U0 "$f" | grep -E '^[-+]' | grep -v -E '^(\+\+\+|---)' | grep -v -E 'uuid|tstamp' | head -5
done
```
Expected: no lines printed except the `== file` headers (only UUID/tstamp churn). If `COCO-CART-FINGERS.kicad_mod` shows finger geometry changes, the regex in `trim_cart_fingers` and in `write_fingers_footprint` disagree; make them identical.

- [ ] **Step 3: Discard the UUID churn and commit nothing**

```bash
git checkout -- breakout libraries
git status --short
```
Expected: clean apart from files from other tasks.

---

### Task 4: Placement, rule areas, silkscreen, and the GUI sync

**Files:**
- Modify: `tools/place_pcb.py` (`PLACEMENT`, new `add_board_extras()`, docstring)
- Modify (via KiCad GUI, then the script): `PiCoCo/PiCoCo.kicad_pcb`
- Test: `kicad-cli pcb render` images; `kicad-cli pcb drc` (sandbox off) for overlaps

**Interfaces:**
- Consumes: refs from Task 2.
- Produces: a board with every footprint placed, the antenna rule area, the HDMI reserve rule area, the silkscreen texts; ready for routing.

- [ ] **Step 1: Sync the PCB from the schematic (user, GUI)**

In KiCad: open `PiCoCo/PiCoCo.kicad_pro`, PCB editor, `Tools → Update PCB from Schematic…`, tick **Delete footprints with no corresponding symbol**, Update. Expected changes: U1 footprint replaced by `Pico-Carrier`; U10 SOIC-24 replaced by SOIC-20W; U15 SC-70 replaced by SOIC-14; JP2, JP3, Q3, Q4, R15..R22, C12..C15, TP7, FID1, FID2 added at origin; C5, C11, R6, JP1 removed; nets renamed. Save. Close the PCB editor before running scripts.

- [ ] **Step 2: Replace `PLACEMENT`**

Coordinates are board mm (KiCad Y down). Board: x 99.59..197.59, y 44.187..99.187, fingers at the bottom. Finger x positions: cart pad n at x = 121.92 + 2.54*ceil(n/2) (pins 3/4 at 127.0, 9 at 134.6, D0..D7 134.6..142.2, A0..A7 144.8..152.4, A8..A12 155.0..160.0, /CTS 160.0, /SCS 167.6, A13/A14 168.9, A15//SLENB 171.5, SND 167.6).

```python
PLACEMENT: dict[str, tuple[float, float, float]] = {
    # Module along the top edge, horizontal: USB end flush with the LEFT board edge,
    # antenna end pointing right; body x 99.6..150.6, y 45.7..66.7. Rotation is verified
    # by the render in step 5 (USB must be at x=99.6); if it comes out mirrored use 270.
    "U1":   (125.1, 56.2, 90),
    # Power block near the +5V finger (x 134.6): LDO, Schottky, bulk caps
    "U14":  (108.0, 92.0, 0), "D2": (115.0, 96.0, 0), "C1": (120.0, 97.5, 0), "C2": (105.0, 84.0, 90),
    "C3":   (113.0, 84.0, 90), "C12": (106.0, 74.0, 0), "R4": (118.0, 70.0, 0),
    # /HALT, /NMI, /CART stages near fingers 3/4/8 (x 127..132)
    "R1": (124.0, 86.0, 0), "Q2": (128.0, 85.0, 0), "R7": (124.0, 80.0, 0), "R8": (128.0, 80.0, 0),
    "R2": (132.0, 86.0, 0), "Q3": (132.0, 80.0, 0), "R15": (136.0, 80.0, 0), "R17": (136.0, 76.0, 0),
    "Q4": (128.0, 74.0, 0), "R16": (132.0, 74.0, 0), "R18": (132.0, 70.0, 0),
    "R3": (140.0, 70.0, 0),
    # Buffer row nearest the fingers, in finger order: data, A0-7, A8-13/RW/CTS, controls
    "U10": (140.0, 92.0, 0), "U11": (154.0, 92.0, 0), "U12": (168.0, 92.0, 0), "U13": (182.0, 92.0, 0),
    "C4": (140.0, 84.0, 90), "C6": (154.0, 84.0, 90), "C7": (168.0, 84.0, 90), "C8": (182.0, 84.0, 90),
    "U15": (175.0, 77.0, 0), "C9": (181.0, 77.0, 90),
    "R11": (175.0, 71.0, 0), "R12": (163.0, 84.0, 0), "JP2": (146.0, 84.0, 0),
    "R9": (190.0, 84.0, 0), "R10": (194.0, 84.0, 90),
    # Sound stage in a line to the SND finger (x 167.6): keep it below U13's row end
    "R19": (188.0, 97.5, 0), "C13": (191.0, 97.5, 0), "R20": (194.0, 97.5, 0), "C14": (194.0, 93.0, 90),
    "R21": (194.0, 89.0, 90), "R22": (190.0, 89.0, 90), "C15": (186.0, 89.0, 90), "TP7": (186.0, 93.0, 0),
    # Between module and the reserved HDMI corner: JP3 by module pin 34, SWD, test points, C10
    "JP3": (152.0, 62.0, 0), "C10": (152.0, 66.0, 90),
    "J_SWD": (160.0, 50.0, 90), "R13": (160.0, 60.0, 0), "R14": (160.0, 64.0, 0),
    "TP1": (166.0, 50.0, 0), "TP2": (166.0, 54.0, 0), "TP3": (166.0, 58.0, 0),
    "TP4": (166.0, 62.0, 0), "TP5": (166.0, 66.0, 0), "TP6": (170.0, 50.0, 0),
    # Fiducials 3 mm in from two diagonal corners
    "FID1": (102.6, 47.2, 0), "FID2": (194.6, 96.2, 0),
}
```
Reserved HDMI corner: x 172.6..197.6, y 44.2..59.2 (nothing placed there). J_SWD reference may be `J_SWD` or `J_SWD1` in the PCB; check with `grep -o '(property "Reference" "J_SWD[0-9]*"' PiCoCo/PiCoCo.kicad_pcb` and use that key.

- [ ] **Step 3: Add `add_board_extras()`**

Appended before the file's final `)`; idempotent via the zone/text names.

```python
def _zone_keepout(name: str, layers: str, pts: list[tuple[float, float]], what: str) -> str:
    poly = " ".join(f"(xy {x:.3f} {y:.3f})" for x, y in pts)
    return (f'  (zone (net 0) (net_name "") (layers {layers}) (name "{name}") (hatch edge 0.5)\n'
            f'    (keepout {what})\n    (polygon (pts {poly}))\n  )\n')

EXTRAS = [
    # Antenna: from the module's pin 20/21 end (x=150.6) 4.92 mm to the right, full module width.
    _zone_keepout("antenna_keepout", '"F.Cu" "B.Cu"',
                  [(150.6, 45.7), (155.6, 45.7), (155.6, 66.7), (150.6, 66.7)],
                  "(tracks not_allowed) (vias not_allowed) (pads not_allowed) (copperpour not_allowed) (footprints not_allowed)"),
    # Reserved HDMI corner: no footprints, routing allowed.
    _zone_keepout("hdmi_reserved", '"F.Cu"',
                  [(172.6, 44.2), (197.6, 44.2), (197.6, 59.2), (172.6, 59.2)],
                  "(tracks allowed) (vias allowed) (pads not_allowed) (copperpour allowed) (footprints not_allowed)"),
]
TEXTS = [  # (text, x, y, layer, size)
    ("HDMI (future)", 185.0, 51.5, "F.SilkS", 1.0),
    ("PiCoCo v2.3  CERN-OHL-S-2.0", 148.0, 70.0, "F.SilkS", 1.0),
    ("github.com/cognitivegears/PiCoCo", 148.0, 72.0, "F.SilkS", 0.8),
    ("JLCJLCJLCJLC", 110.0, 62.0, "B.SilkS", 1.0),
    ("JP2 1-2=HW /OE  2-3=FW", 146.0, 80.5, "F.SilkS", 0.6),
    ("JP3 1-2=E  2-3=AUDIO (Pico2)", 152.0, 59.0, "F.SilkS", 0.6),
    ("no parts under module", 125.0, 62.0, "F.Fab", 1.0),
]

def add_board_extras(text: str) -> str:
    end = text.rstrip().rfind(")")
    body, tail = text[:end], text[end:]
    for z in EXTRAS:
        name = re.search(r'\(name "([^"]+)"', z).group(1)
        if f'(name "{name}")' not in body:
            body = body.rstrip("\n") + "\n" + z
    for t, x, y, layer, size in TEXTS:
        if f'(gr_text "{t}"' not in body:
            body = body.rstrip("\n") + (f'\n  (gr_text "{t}" (at {x} {y} 0) (layer "{layer}")\n'
                                        f'    (effects (font (size {size} {size}) (thickness {size*0.15:.2f})))\n  )\n')
    return body + tail
```
Call it in `main()` after the placement pass, on the same text, before writing. Update the module docstring's layout description (module top-left horizontal, buffer row, reserved corner).

- [ ] **Step 4: Run placement**

Run: `python3 tools/place_pcb.py`
Expected: every ref in `PLACEMENT` reported as moved; none reported missing. A ref reported missing means the GUI sync in step 1 did not create it; redo step 1.

- [ ] **Step 5: Render and check by eye**

```bash
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
$K pcb render --side top --width 1600 --height 900 --output /tmp/top.png PiCoCo/PiCoCo.kicad_pcb
$K pcb render --side bottom --width 1600 --height 900 --output /tmp/bottom.png PiCoCo/PiCoCo.kicad_pcb
```
Read `/tmp/top.png` (the Read tool shows images). Checklist: USB end of U1 at the left edge, "ANT" at the right of the module inside the board, nothing inside the module outline, HDMI corner empty, fingers at the bottom, buffers in a row above the fingers, JP2 next to U10 pin 19, JP3 next to the module's pin 34. If the module is mirrored, change U1's rotation to 270 and rerun step 4.

- [ ] **Step 6: Overlap DRC (sandbox disabled)**

```bash
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
$K pcb drc --format json --severity-error --output /tmp/drc.json PiCoCo/PiCoCo.kicad_pcb
python3 -c "
import json; d=json.load(open('/tmp/drc.json'))
v=d['violations']; print('errors',len(v))
for x in v: print(x['type'], x['description'][:100])
" | grep -v unconnected | head -40
```
Expected before routing: only `unconnected_items` and copper-clearance errors between unrouted items; no `courtyards_overlap`, no `silk_over_copper` on the module. Fix overlaps by nudging `PLACEMENT` and rerunning steps 4-6.

- [ ] **Step 7: Commit**

```bash
git add tools/place_pcb.py PiCoCo/PiCoCo.kicad_pcb
git commit -m "kicad: v2.3 placement, antenna and HDMI rule areas, silkscreen"
```

---

### Task 5: Routing, zones, DRC, parity (user routes; executor verifies)

**Files:**
- Modify (GUI): `PiCoCo/PiCoCo.kicad_pcb`

**Interfaces:**
- Produces: a DRC-clean, parity-clean board.

- [ ] **Step 1: Route in the KiCad GUI (user)**

Net classes are already in `PiCoCo.kicad_pro` (0.2 mm signal, 0.5 mm power, 0.15 mm clearance). Order: power (+5V finger → D2/U14, VSYS_PICO to U1, +3V3 to all buffers), then D0..D7 (fingers → U10 → U1), A0..A13 (fingers → U11/U12 → U1), controls (U13, U15, R11/R12, JP2/JP3), then the rest. Keep the audio stage traces away from buffer outputs. Add ground pours on both layers (fill), stitch with vias near each buffer's GND pin. Do not route inside `antenna_keepout`.

- [ ] **Step 2: DRC with schematic parity (sandbox disabled)**

```bash
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
$K pcb drc --schematic-parity --format json --severity-all --output /tmp/drc.json PiCoCo/PiCoCo.kicad_pcb
python3 -c "
import json; d=json.load(open('/tmp/drc.json'))
print('violations',len(d['violations']),'unconnected',len(d['unconnected_items']),'parity',len(d.get('schematic_parity',[])))
for k in ('violations','unconnected_items','schematic_parity'):
    for x in d.get(k,[])[:20]: print(k, x['severity'], x['type'], x['description'][:100])
"
```
Expected: `violations 0 unconnected 0 parity 0`. Silkscreen warnings over pads are acceptable only if they are on the module's own footprint text; otherwise move the text in `TEXTS` and rerun Task 4 step 4.

- [ ] **Step 3: Render both sides again and read the images** (Task 4 step 5). Check pours reach the buffers and the antenna area is empty copper-wise.

- [ ] **Step 4: Commit**

```bash
git add PiCoCo/PiCoCo.kicad_pcb
git commit -m "kicad: v2.3 routed, zones, DRC and parity clean"
```

---

### Task 6: Fab package: Gerbers, module stencil, JLCPCB BOM and CPL, ordering checklist

**Files:**
- Modify: `tools/gen_fab.sh`
- Create: `tools/jlc_post.py` (CPL/BOM post-processing, stdlib)
- Create: `fab/main/READ-BEFORE-ORDERING.txt` (move and rewrite the current `fab/READ-BEFORE-ORDERING.txt`)
- Test: run the script on the routed board and inspect the CSVs

**Interfaces:**
- Consumes: symbol properties `LCSC`, `MPN`, `DNP` from Task 2.
- Produces: `fab/main/PiCoCo-gerbers.zip`, `fab/main/PiCoCo-stencil-module-F_Paste.gbr`, `fab/main/PiCoCo-BOM-jlc.csv`, `fab/main/PiCoCo-CPL-jlc.csv`.

- [ ] **Step 1: Write `tools/jlc_post.py`**

```python
#!/usr/bin/env python3
"""Post-process kicad-cli outputs into JLCPCB's BOM and CPL formats.
usage: jlc_post.py bom  in.csv out.csv
       jlc_post.py cpl  in.csv out.csv
       jlc_post.py stencil in.kicad_pcb out.kicad_pcb   # give U1's pads an F.Paste layer
"""
import csv, re, sys

# Rotation offsets (degrees, added to KiCad's rotation) so JLCPCB's pick-and-place preview
# shows pin 1 where the footprint has it. Not yet verified against the JLCPCB preview; Task 8
# step 2 fills in offsets and records the verification date in this comment.
ROT = {"SOIC-20W": 0, "SOIC-14": 0, "SOT-23": 0, "SOT-223": 0, "D_SMA": 0, "0805": 0, "CP_Elec": 0}

def bom(src, dst):
    rows = list(csv.DictReader(open(src, newline="")))
    with open(dst, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "MPN"])
        for r in rows:
            w.writerow([r["Value"], r["Reference"], r["Footprint"].split(":")[-1], r.get("LCSC", ""), r.get("MPN", "")])

def cpl(src, dst):
    rows = list(csv.DictReader(open(src, newline="")))
    with open(dst, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in rows:
            off = next((v for k, v in ROT.items() if k in r["Package"]), 0)
            rot = (float(r["Rot"]) + off) % 360
            w.writerow([r["Ref"], f'{float(r["PosX"]):.3f}mm', f'{float(r["PosY"]):.3f}mm',
                        "Top" if r["Side"].lower() in ("top", "front") else "Bottom", f"{rot:.0f}"])

def stencil(src, dst):
    text = open(src).read()
    i = text.index('(property "Reference" "U1"')
    start = text.rfind("(footprint ", 0, i)
    depth, j = 0, start
    while True:
        c = text[j]; depth += (c == "(") - (c == ")"); j += 1
        if depth == 0: break
    fp = text[start:j]
    fp2 = re.sub(r'\(layers "F\.Cu" "F\.Mask"\)', '(layers "F.Cu" "F.Paste" "F.Mask")', fp)
    open(dst, "w").write(text[:start] + fp2 + text[j:])

if __name__ == "__main__":
    {"bom": bom, "cpl": cpl, "stencil": stencil}[sys.argv[1]](sys.argv[2], sys.argv[3])
```

- [ ] **Step 2: Extend `tools/gen_fab.sh`**

Change the default output dir to `$PROJECT_ROOT/fab/main` when no argument is given, and after the existing position export add:

```bash
echo "=> JLCPCB BOM"
"$KICAD_CLI" sch export bom --output "$FAB_DIR/$BASE-BOM-raw.csv" \
    --fields "Value,Reference,Footprint,LCSC,MPN" --labels "Value,Reference,Footprint,LCSC,MPN" \
    --group-by "Value,Footprint,LCSC" --exclude-dnp "$SCH"
python3 "$SCRIPT_DIR/jlc_post.py" bom "$FAB_DIR/$BASE-BOM-raw.csv" "$FAB_DIR/$BASE-BOM-jlc.csv"

echo "=> JLCPCB CPL (top, SMD, no DNP)"
"$KICAD_CLI" pcb export pos --output "$FAB_DIR/$BASE-pos-top.csv" --format csv --units mm \
    --side front --smd-only --exclude-dnp --use-drill-file-origin "$PCB"
python3 "$SCRIPT_DIR/jlc_post.py" cpl "$FAB_DIR/$BASE-pos-top.csv" "$FAB_DIR/$BASE-CPL-jlc.csv"

echo "=> Module stencil (paste on U1 pads only)"
TMP_PCB="$(mktemp -t picoco).kicad_pcb"
python3 "$SCRIPT_DIR/jlc_post.py" stencil "$PCB" "$TMP_PCB"
"$KICAD_CLI" pcb export gerbers --output "$FAB_DIR/stencil-module/" --layers "F.Paste" --no-x2 --use-drill-file-origin "$TMP_PCB"
rm -f "$TMP_PCB"
```
Note: with `--exclude-dnp` the raw BOM still includes rows whose symbols have `in_bom no`? No: `in_bom no` symbols are dropped by KiCad already (U1, P1, JP2, JP3, TP*, J_SWD, FID*). The module-stencil Gerber comes from the temp copy, so the checked-in board keeps no paste on U1. The production `F.Paste` Gerber in `$FAB_DIR/` therefore has paste only on placed parts. Keep the zip step but zip only the production Gerbers (exclude `stencil-module/`).

- [ ] **Step 3: Run it and inspect**

Run: `tools/gen_fab.sh`
Then:
```bash
head -5 fab/main/PiCoCo-BOM-jlc.csv; wc -l fab/main/PiCoCo-BOM-jlc.csv
head -5 fab/main/PiCoCo-CPL-jlc.csv; wc -l fab/main/PiCoCo-CPL-jlc.csv
grep -c "D02" fab/main/stencil-module/*F_Paste.gbr
grep -c "D02" fab/main/PiCoCo-F_Paste.gbp
```
Expected: BOM has one row per distinct value/footprint/LCSC with designators grouped, no DNP refs (R2, R3, Q3, Q4, R15..R18, C12, C15 absent), U1/P1/JP*/TP*/FID* absent. CPL lists every placed SMD part exactly once, Layer `Top`. The module stencil Gerber has flashes (55 pads); the production paste Gerber has none at the module's coordinates (open both in KiCad's Gerber viewer, `gerbview`, and look).

- [ ] **Step 4: Write `fab/main/READ-BEFORE-ORDERING.txt`**

`git mv fab/READ-BEFORE-ORDERING.txt fab/main/READ-BEFORE-ORDERING.txt`, then rewrite the ASSEMBLY section and the settings block to:

```
JLCPCB ORDER SETTINGS (v2.3, 2026-09)
  Base material FR-4 / Layers 2 / Dimensions 98 x 55 mm / Thickness 1.6 mm (critical)
  Surface finish: ENIG (all pads gold; fingers get hard gold below, never HASL)
  Outer copper 1 oz
  Gold fingers: YES (critical) / Bevel: 30 deg (critical) / Gold thickness: 1 um min (2 um better)
  Remove order number: "Specify a location" (the JLCJLCJLCJLC text on the bottom silkscreen)
  Castellated holes: No / Impedance: No
  SMT assembly: Top side, Economic. Files: fab/main/PiCoCo-BOM-jlc.csv, fab/main/PiCoCo-CPL-jlc.csv
  Extended parts on this board: SN74LVC245A x4 (one part number), SN74LVC00A x1. Confirm stock.
  Parts NOT placed (by design): U1 module (yours), J_SWD header (hand), R2 R3 C12 C15 Q3 Q4 R15 R16 R17 R18 (DNP).
  Check the placement preview: every SOIC/SOT pin-1 dot matches the silkscreen. If any part is
  rotated, fix the ROT table in tools/jlc_post.py, rerun tools/gen_fab.sh, re-upload.
BUDGET VARIANT (bare boards, hobbyist): same Gerbers, ENIG, gold fingers NO, bevel NO.
  ENIG fingers wear after tens of insertions; fine for a lightly used cart.
PLUS-W INSTALL: fab/main/stencil-module/*F_Paste.gbr is a paste stencil for the 55 module pads
  (15 hidden). Hot air or a paste syringe; or mount on 2x20 headers and forgo the 15 pads.
MODULE: solder the Pico 2 flat on the castellations, or on 2x20 headers. Nothing under it.
```
Keep the "WHY THESE SETTINGS MATTER" and post-receipt checks from the old file.

- [ ] **Step 5: Commit**

```bash
git add tools/gen_fab.sh tools/jlc_post.py fab/main/READ-BEFORE-ORDERING.txt fab/main/*.csv fab/main/*.zip fab/main/stencil-module
git commit -m "fab: JLCPCB BOM/CPL export, module stencil, v2.3 ordering checklist"
```
(If v2.2 Gerbers are tracked at the `fab/` root, remove them in the same commit with `git rm fab/*.g?? fab/*.drl`; the breakout and cobbler folders stay.)

---

### Task 7: Documentation and CLAUDE.md rules

**Files:**
- Modify: `docs/hardware-design.md`, `docs/BOM.md`, `docs/kicad-workflow.md`, `CLAUDE.md`, `docs/firmware-architecture.md`, `docs/RP2350B_IDEAS.md`

- [ ] **Step 1: `docs/hardware-design.md`**

Block diagram: U10 line becomes `D0..D7 ── P1[10..17] ◄──► [U10 SN74LVC245A @3.3V, cart on B] ◄──► GP0..GP7`; /OE gate block becomes `U15 74LVC00: SEL_N = NAND(CTS_BUF,SCS_BUF); OE_BUS_RAW = NAND(SEL_N,E_BUF)`; add `SND ── P1[35] ◄── R21/R22 ◄── 2-pole RC ◄── AUDIO_PWM (JP3: hdr34 on Pico 2 / GP34 on Plus-W)`. §2 component list: replace U10, U15 rows; add JP2, JP3, Q3, Q4, R15..R22, C12..C15, TP7, FID1/2; delete C5, C11, R6, JP1. §3.2: add a "Plus-W" column (header 31/32/34 = GP40/41/42) and the pad-grid table from spec §3.2. §4.1: rewrite for the LVC245 at 3.3 V (A side = Pico). §4.2: U13 channel list from spec. §4.3: NAND-NAND text, delete the AND description. New §4.6 Sound stage (spec §3.2 Sound). §7: fiducials and TP7. §9: remove items now provisioned (NMI/CART drive, A14/A15, Q). Add a line under §1: "Reserved top-right corner for a future HDMI-A (Plus-W only, see spec §10)."

- [ ] **Step 2: `docs/BOM.md`**

Rewrite from spec §3.3: columns Ref, MPN, Package, LCSC, JLC library (Basic/Extended), Lifecycle, Notes. Remove the wrong numbers (C9963, C35952, C130103). Add a paragraph "Ordering" pointing to `fab/main/READ-BEFORE-ORDERING.txt` and the cost estimate (5 PCBs + 2 assembled, hard gold: about $90 to $130; ENIG fingers: subtract $30 to $50).

- [ ] **Step 3: `docs/kicad-workflow.md`**

Header: revision v2.3, list what changed. §1 table: `Pico-Carrier` footprint/symbol are generated by `gen_schematic.py`; `check_library.py`. §2.3: the expected Update-PCB diff from Task 4 step 1. New §2.6 "Paste and stencils": module pads carry no paste; `stencil-module/` is the Plus-W stencil. New §2.7 "JLCPCB files": BOM/CPL, the rotation table.

- [ ] **Step 4: `CLAUDE.md`**

Hard rules: replace the `SN74LVC8T245` sentence with "Use `SN74LVC245A` at 3.3 V for all four buffers; U10's A side (pins 2..9) is the Pico, B side (18..11) is the cart." Replace the U15 quirk bullet with: "U15 is a 74LVC00 (SOIC-14) wired NAND-NAND: `SEL_N = NAND(CTS_BUF,SCS_BUF)`, `OE_BUS_RAW = NAND(SEL_N,E_BUF)`; gates 3/4 grounded." Net-name rule: add `_DBG` to the banned suffixes; mention `_BUF` on all buffered nets including `RESET_BUF`. Add: "JP2 default 1-2 (hardware /OE); JP3 default 1-2 (E on header pin 34). `gen_schematic.py` also writes `Pico-Carrier` and `74LVC00` into the local library; never hand-edit those two." Layout section: `tools/check_library.py`, `tools/jlc_post.py`, `fab/main/`.

- [ ] **Step 5: `docs/firmware-architecture.md` and `docs/RP2350B_IDEAS.md`**

Firmware doc: add a subsection "Plus-W pin plan (board v2.3, not implemented)" with the table from spec §3.2 plus header 31/32/34 = GP40/41/42 and the note that E also arrives on pad GP26. RP2350B_IDEAS §13.5: mark "Underside pad pitch" and "which end faces the board edge" as closed by the spec; leave the hand-soldering and tx-power items.

- [ ] **Step 6: Commit**

```bash
git add docs/hardware-design.md docs/BOM.md docs/kicad-workflow.md CLAUDE.md docs/firmware-architecture.md docs/RP2350B_IDEAS.md
git commit -m "docs: v2.3 hardware doc, BOM with LCSC numbers, KiCad workflow, CLAUDE.md rules"
```

---

### Task 8: Order (user) and record

**Files:**
- Modify: `fab/main/READ-BEFORE-ORDERING.txt` (append the order record), `docs/BOM.md` (actual prices)

- [ ] **Step 1: Upload `fab/main/PiCoCo-gerbers.zip` to jlcpcb.com**, set the options from the checklist, enable SMT assembly, upload the BOM and CPL. In the parts step confirm the two Extended parts and pick the C12 alternative only if you populate it (default: leave DNP). In the placement preview compare pin-1 marks with the silkscreen for U10..U15, Q2, D2, U14.

- [ ] **Step 2: If any rotation is wrong**, set the offset in `ROT` in `tools/jlc_post.py`, write the verification date into the comment, rerun `tools/gen_fab.sh`, re-upload the CPL.

- [ ] **Step 3: Place the order** (5 PCBs, 2 or 5 assembled). Append to `fab/main/READ-BEFORE-ORDERING.txt`: date, order number, options chosen, price, and any rotation offsets applied. Put the real per-order price in `docs/BOM.md`.

- [ ] **Step 4: Commit and tag**

```bash
git add fab/main/READ-BEFORE-ORDERING.txt docs/BOM.md tools/jlc_post.py fab/main/PiCoCo-CPL-jlc.csv
git commit -m "fab: v2.3 ordered (JLCPCB order number, options, price)"
git tag -a pcb-v2.3-ordered -m "Main board v2.3 ordered from JLCPCB"
```
