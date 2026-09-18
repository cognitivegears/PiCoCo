#!/usr/bin/env python3
"""
Generate PiCoCo/PiCoCo.kicad_sch from scratch.

Strategy:
  * Parse each symbol's pin positions from its .kicad_sym source.
  * Place each component at a grid position.
  * For every net-assigned pin, emit a global label at that pin's
    world-space endpoint with the correct orientation so the label
    touches the pin tip (KiCad considers this "connected" for netlist
    purposes, no wire needed).
  * Power pins (GND, +5V, +3V3) get `no_connect` markers suppressed
    by instead using the label-at-pin technique with a power-rail net.
  * True no-connect pins (unused Pico GPIOs, NC bus pins on the cart,
    etc.) get `(no_connect (at x y))` markers.

Result: a netlist-grade schematic, ERC-clean for all primary pins.
Visual polish is left to KiCad UI.
"""

from __future__ import annotations

import re
import uuid
from pathlib import Path
from textwrap import indent

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KICAD_STOCK = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")
SCH_OUT = PROJECT_ROOT / "PiCoCo" / "PiCoCo.kicad_sch"
LOCAL_SYMS = PROJECT_ROOT / "libraries" / "PiCoCo.kicad_sym"


def u() -> str:
    return str(uuid.uuid4())


# ---------------------------------------------------------------------------
# Symbol extraction and pin parsing
# ---------------------------------------------------------------------------

def extract_symbol(lib_path: Path, sym_name: str) -> str:
    """Extract a single top-level `(symbol "name" ...)` block."""
    text = lib_path.read_text()
    pattern = re.compile(
        r'^([ \t]+)\(symbol\s+"' + re.escape(sym_name) + r'"',
        re.MULTILINE,
    )
    m = pattern.search(text)
    if not m:
        raise RuntimeError(f"symbol {sym_name} not found in {lib_path}")
    start = m.start()
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise RuntimeError(f"unbalanced parens extracting {sym_name}")


def rewrite_symbol_libid(sym_block: str, new_libid: str) -> str:
    return re.sub(
        r'^(\s*)\(symbol\s+"[^"]+"',
        f'\\1(symbol "{new_libid}"',
        sym_block,
        count=1,
    )


def _extract_pin_blocks(sym_block: str) -> list[str]:
    """Return each balanced `(pin ...)` block inside a symbol definition."""
    out = []
    i = 0
    while True:
        idx = sym_block.find("(pin ", i)
        if idx == -1:
            return out
        # Walk balanced
        depth = 0
        j = idx
        while j < len(sym_block):
            c = sym_block[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    out.append(sym_block[idx:j + 1])
                    i = j + 1
                    break
            j += 1
        else:
            return out


_PIN_HEADER = re.compile(
    r'\(pin\s+(\w+)\s+(\w+)\s+'
    r'\(at\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(\d+)\)\s+'
    r'\(length\s+([\d.]+)\)'
)
_PIN_NAME = re.compile(r'\(name\s+"([^"]*)"')
_PIN_NUMBER = re.compile(r'\(number\s+"([^"]+)"')


def retype_pins(sym_block: str, pin_numbers: set[str], new_type: str) -> str:
    """Rewrite the electrical type of specific numbered pins (embedded copy only).

    Used so the 74LS245 stock symbol's A/B bus pins don't trip KiCad's
    default tri_state-vs-output/power_input pin_to_pin ERC warning when
    wired to the (fixed-direction, DIR-tied) cart edge and to GND: real
    hardware has no contention here, but the stock symbol types both bus
    sides `tri_state` generically. Does not touch the source .kicad_sym.
    """
    out = sym_block
    seen = set()
    for block in _extract_pin_blocks(sym_block):
        nu = _PIN_NUMBER.search(block)
        if not nu or nu.group(1) not in pin_numbers:
            continue
        new_block = re.sub(r'^\(pin\s+\w+(\s+\w+)', f'(pin {new_type}\\1', block, count=1)
        assert new_block != block, f"retype produced no change for pin {nu.group(1)}"
        out = out.replace(block, new_block, 1)
        seen.add(nu.group(1))
    missing = pin_numbers - seen
    assert not missing, f"retype_pins: pins not found: {missing}"
    return out


def parse_pins(sym_block: str) -> list[dict]:
    """Return a list of pins with their symbol-local positions."""
    pins = []
    for block in _extract_pin_blocks(sym_block):
        h = _PIN_HEADER.search(block)
        if not h:
            continue
        etype, shape, x, y, angle, length = h.groups()
        nm = _PIN_NAME.search(block)
        nu = _PIN_NUMBER.search(block)
        if not nu:
            continue
        pins.append({
            "num": nu.group(1),
            "name": nm.group(1) if nm else "",
            "elec_type": etype,
            "shape": shape,
            "x": float(x),
            "y": float(y),
            "angle": int(angle),
            "length": float(length),
        })
    return pins


# ---------------------------------------------------------------------------
# Symbol registry
# ---------------------------------------------------------------------------

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
    ("Jumper:SolderJumper_2_Open", KICAD_STOCK / "Jumper.kicad_sym", "SolderJumper_2_Open"),
    ("Mechanical:Fiducial", KICAD_STOCK / "Mechanical.kicad_sym", "Fiducial"),
    ("Connector:TestPoint", KICAD_STOCK / "Connector.kicad_sym", "TestPoint"),
    ("power:+5V", KICAD_STOCK / "power.kicad_sym", "+5V"),
    ("power:+3V3", KICAD_STOCK / "power.kicad_sym", "+3V3"),
    ("power:GND", KICAD_STOCK / "power.kicad_sym", "GND"),
    ("power:PWR_FLAG", KICAD_STOCK / "power.kicad_sym", "PWR_FLAG"),
]


# 74LS245 A1..A8/B1..B8 (pins 2-9, 11-18) are typed tri_state in the stock
# symbol; DIR (1) and CE/OE (19) stay input, VCC (20)/GND (10) stay power_in.
# Retyped to passive in the embedded copy only -- see retype_pins() docstring.
_LVC245_BUS_PINS = {str(n) for n in list(range(2, 10)) + list(range(11, 19))}


def load_symbols(symbols=None) -> tuple[str, dict[str, dict]]:
    """Return (embedded_block_text, pin_map). pin_map: libid -> {num: pin}."""
    blocks = []
    pin_map: dict[str, dict] = {}
    for libid, path, name in (symbols or SYMBOLS):
        block = extract_symbol(path, name)
        if libid == "74xx:74LS245":
            block = retype_pins(block, _LVC245_BUS_PINS, "passive")
        pins = parse_pins(block)
        # Note: pin_map key is the short symbol name as found in the source,
        # but we also index by libid for convenience.
        pin_map[libid] = {p["num"]: p for p in pins}
        block = rewrite_symbol_libid(block, libid)
        blocks.append(block)
    body = "\n".join(blocks)
    return f"  (lib_symbols\n{indent(body, '  ')}\n  )", pin_map


# ---------------------------------------------------------------------------
# Library items the v2.3 schematic depends on (written idempotently)
# ---------------------------------------------------------------------------
PRETTY = PROJECT_ROOT / "libraries" / "PiCoCo.pretty"

PAD_GRID = {  # RP2350B-Plus-W underside pads, footprint frame (RP2350B_IDEAS §13.4)
    "GP26": (-5.08, 22.40), "GP29": (-2.54, 22.40), "GP32": (0.0, 22.40), "GP35": (2.54, 22.40), "GP45": (5.08, 22.40),
    "GP25": (-5.08, 19.86), "GP28": (-2.54, 19.86), "GP31": (0.0, 19.86), "GP34": (2.54, 19.86), "GP44": (5.08, 19.86),
    "GP24": (-5.08, 17.32), "GP27": (-2.54, 17.32), "GP30": (0.0, 17.32), "GP33": (2.54, 17.32), "GP43": (5.08, 17.32),
}


def _uid(item: str) -> str:
    """Deterministic tstamp so regenerating Pico-Carrier is byte-identical."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"picoco:Pico-Carrier:{item}"))


def _top_level_items(body: str) -> list[str]:
    """Return the balanced top-level s-expr children inside an s-expr node.

    Same helper as gen_breakout.py's `_top_level_items`, duplicated here
    (rather than imported) because gen_breakout.py requires KiCad's pcbnew
    module just to import.
    """
    items, depth, start = [], 0, None
    for i, c in enumerate(body):
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                items.append(body[start:i + 1])
    return items


def write_carrier_footprint() -> None:
    """Pico-Carrier = RPi_Pico_SMD_TH + the 15 Plus-W pads + antenna courtyard.

    No paste anywhere. Pico 2's own SWD debug castellations (pads 41/42/43 =
    SWCLK/GND/SWDIO, the row at Y=23.9) sit directly under the Plus-W
    GP29/GP32/GP35 pads (Y=22.40, same X positions) and cannot coexist: the
    drilled holes land 0.24 mm from even a 1.5 mm pad, inside the 0.25 mm
    min_hole_clearance rule. Drop 41/42/43 entirely -- a carrier board
    reaches SWD at the module's own castellations directly, not through
    this footprint.

    Items are dropped/rewritten by splitting the footprint body into its
    balanced top-level s-expressions (not a cross-item regex): a regex
    spanning from the first "(fp_line" to the first "(layer \"F.CrtYd\")"
    previously ate every silkscreen/keepout item in between.
    """
    src = (PRETTY / "RPi_Pico_SMD_TH.kicad_mod").read_text()
    assert src.startswith('(footprint "RPi_Pico_SMD_TH"')
    assert "F.Paste" not in src, "source footprint unexpectedly has paste"

    head_end = src.index("\n", src.index("(footprint"))
    header = src[:head_end].replace('(footprint "RPi_Pico_SMD_TH"', '(footprint "Pico-Carrier"', 1)
    body = src[head_end:src.rstrip().rfind(")")]

    kept = []
    for it in _top_level_items(body):
        kind = it.split(None, 1)[0].lstrip("(")
        if kind == "pad" and re.match(r'\(pad "4[123]" ', it):
            continue  # SWD debug pads collide with the Plus-W grid (see docstring)
        if kind == "fp_text" and re.match(r'\(fp_text user "(SWCLK|SWDIO)"', it):
            continue  # silkscreen labels for the removed debug pads 41-43
        if kind in ("fp_line", "fp_rect", "fp_arc", "fp_circle") and '(layer "F.CrtYd")' in it:
            continue  # inherited courtyard; replaced with the Plus-W envelope below
        if kind == "model":
            continue  # carrier has no 3D model
        if kind == "descr":
            it = ('(descr "Carrier land pattern for Raspberry Pi Pico 2 or Waveshare '
                  'RP2350B-Plus-W: 2x20 castellated/THT header plus the Plus-W 3x5 '
                  'underside pad grid; no debug pads (collide with the grid)")')
        elif kind == "tags":
            it = '(tags "Pico 2 RP2350B-Plus-W carrier")'
        elif kind == "fp_text" and it.startswith('(fp_text value "RPi_Pico_SMD_TH"'):
            it = it.replace('"RPi_Pico_SMD_TH"', '"Pico-Carrier"', 1)
        kept.append(it)

    extra = []
    for num, (x, y) in PAD_GRID.items():
        # GP29/GP32/GP35 sit closest to where the (now-removed) debug pads
        # were; keep them 1.4x1.4 so a bare Pico 2's own debug pad copper
        # (edge near Y=23.05) still clears carrier copper by >=0.15 mm.
        size = 1.4 if num in ("GP29", "GP32", "GP35") else 1.8
        extra.append(f'(pad "{num}" smd rect (at {x} {y}) (size {size} {size}) (layers "F.Cu" "F.Mask") (tstamp {_uid(num)}))')
    # Plus-W envelope: 51 mm body (+-25.5) plus 4.92 mm antenna past the pin 20/21 end (+Y).
    extra.append('(fp_rect (start -10.5 -25.5) (end 10.5 30.42) (stroke (width 0.05) (type default)) (fill none) (layer "F.CrtYd") (tstamp %s))' % _uid("courtyard"))
    extra.append('(fp_rect (start -10.5 25.5) (end 10.5 30.42) (stroke (width 0.1) (type default)) (fill none) (layer "F.Fab") (tstamp %s))' % _uid("antenna-fab-rect"))
    extra.append('(fp_text user "ANT antenna keepout (Plus-W)" (at 0 28) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))) (tstamp %s))' % _uid("antenna-fab-text"))
    extra.append('(fp_text user "USB" (at 0 -23) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))) (tstamp %s))' % _uid("usb-text"))
    extra.append('(fp_text user "ANT" (at 0 27.5) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))) (tstamp %s))' % _uid("ant-text"))

    text = header + "\n  " + "\n  ".join(kept + extra) + "\n)\n"
    (PRETTY / "Pico-Carrier.kicad_mod").write_text(text)


# Shared with gen_breakout.py's write_fingers_footprint (same fab rule).
FINGER_TRIM = (r"\(at ([\d.]+) -5\.207\) \(size 1\.27 9\.525\)", r"(at \1 -5.635) (size 1.27 8.67)")


def fix_cart_footprint() -> None:
    """Fingers end 1.30 mm from the edge (JLCPCB 30 deg bevel is 1.13 mm deep).

    Also drops the MTG1 mounting-hole pad (11 mm pad / 8 mm drill): the v2.3
    module's body sits directly over that corner regardless of U1's rotation
    (see docs/superpowers/sdd/2026-09-17-main-board-v2.3/task-4-report.md),
    producing hole-to-hole and keepout violations no placement nudge can
    clear. Ruling: remove it rather than accept no mounting screw silently.
    Idempotent (both fixes are no-ops on an already-fixed file).
    """
    p = PRETTY / "COCO-CART-2.1X1.75.kicad_mod"
    src = p.read_text()
    out = re.sub(*FINGER_TRIM, src)

    head_end = out.index("\n", out.index("(footprint"))
    body = out[head_end:out.rstrip().rfind(")")]
    for it in _top_level_items(body):
        kind = it.split(None, 1)[0].lstrip("(")
        if kind == "pad" and re.match(r'\(pad "MTG1" ', it):
            # Structural removal via the balanced item found above (not a
            # hardcoded "\n  " indent assumption): locate its real span in
            # `out` and drop it plus its own leading line, whatever that
            # indent actually is.
            item_start = out.index(it)
            item_end = item_start + len(it)
            line_start = out.rfind("\n", 0, item_start) + 1
            out = out[:line_start] + out[item_end:]
            break

    if out != src:
        p.write_text(out)


def fix_cart_symbol() -> None:
    """Drop the MTG@1/MTG1 pin from the base COCO-CART symbol: its matching
    footprint pad was removed in fix_cart_footprint() (see that function's
    docstring). Idempotent.
    """
    sym = extract_symbol(LOCAL_SYMS, "COCO-CART")
    for block in _extract_pin_blocks(sym):
        if '(number "MTG1"' in block:
            whole = "\n      " + block
            assert sym.count(whole) == 1, "MTG1 pin block not unique"
            sym = sym.replace(whole, "")
            _replace_or_append_symbol("COCO-CART", sym)
            break


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
    sym = sym.replace('(text "Raspberry Pi Pico" (at 0 21.59 0)', '(text "Pico 2 / Plus-W" (at 0 21.59 0)')
    # Reference sat at Y=27.94, inside the new top-edge pins' 26.67..29.21
    # strip; move it clear above the pin tips (Y=29.21).
    sym = sym.replace('(property "Reference" "U" (at -13.97 27.94 0)', '(property "Reference" "U" (at -13.97 33.02 0)')
    # Pico symbol names these pins for their Pico 2 ADC function; on the
    # Plus-W module the same physical header pins (31/32/34) carry GP40/41/42
    # instead (RP2350B_IDEAS.md §13.1), so the carrier's silkscreen needs both.
    for old, new in (("GPIO26_ADC0", "GP26/GP40"), ("GPIO27_ADC1", "GP27/GP41"), ("GPIO28_ADC2", "GP28/GP42")):
        assert f'(name "{old}"' in sym, f"pin name {old} not found in Pico symbol"
        sym = sym.replace(f'(name "{old}"', f'(name "{new}"', 1)
    # Drop the SWD debug pins (41=SWCLK, 42=GND, 43=SWDIO): the matching
    # footprint pads collide with the Plus-W grid and were removed in
    # write_carrier_footprint(); see that function's docstring.
    for num in ("41", "42", "43"):
        for block in _extract_pin_blocks(sym):
            if f'(number "{num}"' in block:
                # Remove the preceding "\n      " indent too, else a blank
                # 6-space line is left behind where the pin used to be.
                whole = "\n      " + block
                assert sym.count(whole) == 1, f"pin block {num} not unique"
                sym = sym.replace(whole, "")
                break
        else:
            raise RuntimeError(f"pin {num} not found to remove")
    # Pin 38 (GND) is mistyped `bidirectional` in the base Pico symbol (every
    # other GND pin there is correctly `power_in`); retype it in this derived
    # copy only -- the base "Pico" symbol block is left untouched -- so ERC's
    # pin_to_pin matrix doesn't flag it against a cart-edge GND power_output pin.
    sym = retype_pins(sym, {"38"}, "power_in")
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
    fix_cart_footprint()
    fix_cart_symbol()
    write_carrier_symbol()
    write_lvc00_symbol()


# ---------------------------------------------------------------------------
# Placement helpers
# ---------------------------------------------------------------------------

def world_pin_pos(comp_x: float, comp_y: float, comp_rot: int, mirror: str,
                  pin_x: float, pin_y: float, pin_angle: int
                  ) -> tuple[float, float, int]:
    """Compute world-space (x, y, angle) for a pin on a placed symbol.

    KiCad schematic coordinates: +x right, +y down.
    Symbol-local coordinates: +x right, +y up (math convention).
    So when placing a symbol, we FLIP y.

    comp_rot is 0 / 90 / 180 / 270.
    mirror is "", "x", or "y".
    """
    x, y = pin_x, pin_y

    # Apply mirror first (symbol-local space)
    if mirror == "x":
        y = -y
    elif mirror == "y":
        x = -x

    # Apply rotation in symbol-local coords (counterclockwise in math sense)
    if comp_rot == 0:
        rx, ry = x, y
    elif comp_rot == 90:
        rx, ry = -y, x
    elif comp_rot == 180:
        rx, ry = -x, -y
    elif comp_rot == 270:
        rx, ry = y, -x
    else:
        raise ValueError(f"bad rot {comp_rot}")

    # Translate to world, flipping y because KiCad schematic y is inverted
    wx = comp_x + rx
    wy = comp_y - ry

    # Compute world pin angle
    wang = (pin_angle + comp_rot) % 360
    if mirror == "x":
        wang = (-wang) % 360
    elif mirror == "y":
        wang = (180 - wang) % 360

    return wx, wy, wang


def label_orientation_for_pin(pin_angle: int) -> tuple[int, str]:
    """Return (label_rotation, justify) such that the label points AWAY
    from the component along the pin's axis.

    Pin angle 0 means pin points right (body is to the left), so the label
    should extend to the RIGHT (rot 0, justify left).
    Pin angle 180 means pin points left → label extends left (rot 180, just right).
    Pin angle 90 → label extends up (rot 90, justify left) — no, KiCad labels
      have rotations 0/90/180/270. For vertical text, rot=90 means text rotated
      90° counterclockwise (reads from bottom to top).
    """
    if pin_angle == 0:
        return 0, "left"
    if pin_angle == 90:
        return 90, "left"
    if pin_angle == 180:
        return 180, "right"
    if pin_angle == 270:
        return 270, "right"
    return 0, "left"


SHEET_UUID = str(uuid.uuid4())
PROJECT_NAME = "PiCoCo"   # gen_breakout.py overrides this


# ---------------------------------------------------------------------------
# Output generators
# ---------------------------------------------------------------------------

def symbol_instance(
    lib_id: str, ref: str, value: str,
    x: float, y: float, rot: int = 0, mirror: str = "",
    footprint: str = "", unit: int = 1,
    in_bom: str = "yes", on_board: str = "yes",
    dnp: bool = False, lcsc: str = "", mpn: str = "",
) -> str:
    sym_uuid = u()
    mir = f"(mirror {mirror}) " if mirror else ""
    props = []
    props.append(
        f'    (property "Reference" "{ref}" (at {x:.2f} {y - 12.7:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)))\n'
        f'    )'
    )
    props.append(
        f'    (property "Value" "{value}" (at {x:.2f} {y + 12.7:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)))\n'
        f'    )'
    )
    props.append(
        f'    (property "Footprint" "{footprint}" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )'
    )
    props.append(
        f'    (property "Datasheet" "~" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )'
    )
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
    return (
        f'  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} {rot}) {mir}(unit {unit})\n'
        f'    (in_bom {in_bom}) (on_board {on_board}) (dnp {"yes" if dnp else "no"})\n'
        f'    (uuid {sym_uuid})\n'
        + "\n".join(props) + "\n"
        f'    (instances\n'
        f'      (project "{PROJECT_NAME}"\n'
        f'        (path "/{SHEET_UUID}"\n'
        f'          (reference "{ref}") (unit {unit})\n'
        f'        )\n'
        f'      )\n'
        f'    )\n'
        f'  )'
    )


def global_label(text: str, x: float, y: float, rot: int, justify: str,
                 shape: str = "bidirectional") -> str:
    return (
        f'  (global_label "{text}" (shape {shape}) (at {x:.2f} {y:.2f} {rot}) (fields_autoplaced)\n'
        f'    (effects (font (size 1.27 1.27)) (justify {justify}))\n'
        f'    (uuid {u()})\n'
        f'  )'
    )


def no_connect(x: float, y: float) -> str:
    return f'  (no_connect (at {x:.2f} {y:.2f}) (uuid {u()}))'


_PWR_COUNTER = [0]


def power_port(lib_id: str, x: float, y: float, rot: int = 0) -> str:
    # KiCad only treats a reference as annotated if it ends in digits;
    # random hex suffixes made the GUI demand re-annotation before parity checks.
    _PWR_COUNTER[0] += 1
    ref = f"#PWR{_PWR_COUNTER[0]:04d}"
    value = lib_id.split(":", 1)[1]
    return (
        f'  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} {rot}) (unit 1)\n'
        f'    (in_bom no) (on_board yes) (dnp no)\n'
        f'    (uuid {u()})\n'
        f'    (property "Reference" "{ref}" (at {x:.2f} {y - 3.81:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )\n'
        f'    (property "Value" "{value}" (at {x:.2f} {y - 2.54:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)))\n'
        f'    )\n'
        f'    (property "Footprint" "" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )\n'
        f'    (property "Datasheet" "" (at {x:.2f} {y:.2f} 0)\n'
        f'      (effects (font (size 1.27 1.27)) hide)\n'
        f'    )\n'
        f'    (instances\n'
        f'      (project "{PROJECT_NAME}"\n'
        f'        (path "/{SHEET_UUID}"\n'
        f'          (reference "{ref}") (unit 1)\n'
        f'        )\n'
        f'      )\n'
        f'    )\n'
        f'  )'
    )


# ---------------------------------------------------------------------------
# Component placer
# ---------------------------------------------------------------------------

class Sheet:
    def __init__(self, pin_map: dict[str, dict]):
        self.pin_map = pin_map
        self.components: list[str] = []
        self.labels: list[str] = []
        self.no_connects: list[str] = []
        self.powers: list[str] = []
        self._ref_counter: dict[str, int] = {}
        self.placed: list[dict] = []

    def place(
        self, lib_id: str, ref: str, value: str, x: float, y: float,
        rot: int = 0, mirror: str = "",
        pin_nets: dict[str, str] | None = None,
        footprint: str = "",
        power_port_nets: dict[str, str] | None = None,
        dnp: bool = False, lcsc: str = "", mpn: str = "", in_bom: str = "yes",
    ) -> None:
        """Place a component and wire its pins via global labels.

        pin_nets: {pin_number: net_name}. Any pin not in this dict gets a
        `no_connect` marker. Pin "name" strings that are "+5V", "+3V3",
        or "GND" get a power-port instead of a global label (cleaner look).
        """
        self.placed.append({"ref": ref, "lib_id": lib_id, "value": value, "pin_nets": dict(pin_nets or {}),
                            "dnp": dnp, "lcsc": lcsc, "in_bom": in_bom})
        self.components.append(
            symbol_instance(lib_id, ref, value, x, y, rot, mirror, footprint,
                            in_bom=in_bom, dnp=dnp, lcsc=lcsc, mpn=mpn)
        )
        pins = self.pin_map[lib_id]
        pin_nets = pin_nets or {}
        for num, pin in pins.items():
            wx, wy, wang = world_pin_pos(
                x, y, rot, mirror, pin["x"], pin["y"], pin["angle"]
            )
            net = pin_nets.get(num)
            if net is None:
                # Emit no_connect if the pin is not marked as power or
                # already handled upstream.
                self.no_connects.append(no_connect(wx, wy))
                continue
            if net in ("+5V", "+3V3", "GND"):
                libid = f"power:{net}"
                self.powers.append(power_port(libid, wx, wy, wang))
            else:
                lrot, ljust = label_orientation_for_pin(wang)
                shape = "input" if pin["elec_type"] == "output" else (
                    "output" if pin["elec_type"] == "input" else
                    "bidirectional"
                )
                self.labels.append(
                    global_label(net, wx, wy, lrot, ljust, shape)
                )


# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------

def build() -> tuple[str, "Sheet"]:
    lib_block, pin_map = load_symbols()
    s = Sheet(pin_map)
    LVC245 = ("74xx:74LS245", "SN74LVC245A", "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm", "C571201", "SN74LVC245ADWR")
    R0805 = "Resistor_SMD:R_0805_2012Metric"
    C0805 = "Capacitor_SMD:C_0805_2012Metric"
    RLC = {"33": "C17634", "100": "C17408", "1k": "C17513", "2.2k": "C17520", "4.7k": "C17673", "10k": "C17414", "100k": "C149504", "0": "C17477"}  # C17407 discontinued 2026-09; C17477 (0805 0R) to confirm at order
    CLC = {"10nF": "C1710", "100nF": "C49678", "1uF": "C28323", "10uF": "C15850", "22uF": "C45783"}  # C28323 (0805 1uF) to confirm at order

    def res(ref, value, x, y, a, b, dnp=False):
        s.place("Device:R", ref, value, x, y, pin_nets={"1": a, "2": b}, footprint=R0805,
                dnp=dnp, lcsc=RLC[value], mpn=f"0805 {value} 1%")

    def cap(ref, value, x, y, a, b, dnp=False):
        s.place("Device:C", ref, value, x, y, pin_nets={"1": a, "2": b}, footprint=C0805,
                dnp=dnp, lcsc=CLC[value], mpn=f"0805 {value}")

    # ---------- U1: module (Pico 2 or RP2350B-Plus-W) ----------
    # No SWD header: pins 41/42/43 (Pico 2's own SWD castellations) are not
    # present on Pico-Carrier -- they collide with the Plus-W pad grid (see
    # write_carrier_footprint()). Debug via the module's own debug pads.
    pico_nets = {
        "1": "D0", "2": "D1", "3": "GND", "4": "D2", "5": "D3", "6": "D4", "7": "D5", "8": "GND",
        "9": "D6", "10": "D7", "11": "A0_BUF", "12": "A1_BUF", "13": "GND", "14": "A2_BUF",
        "15": "A3_BUF", "16": "A4_BUF", "17": "A5_BUF", "18": "GND", "19": "A6_BUF", "20": "A7_BUF",
        "21": "A8_BUF", "22": "A9_BUF", "23": "GND", "24": "A10_BUF", "25": "A11_BUF",
        "26": "A12_BUF", "27": "A13_BUF", "28": "GND", "29": "RW_BUF", "30": "PICO_RUN",
        "31": "OE_BUS", "32": "HALT_GATE", "33": "GND", "34": "PICO_P34",
        "37": "PICO_3V3_EN", "38": "GND", "39": "VSYS_PICO",
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
            footprint="Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", lcsc="C485072", mpn="SN74LVC00ADR")  # LCSC checked 2026-09-17: TI, Active, 4225 in stock

    # ---------- JP2: U10 /OE source; JP3: header pin 34 = E or audio ----------
    SJ = "Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm"
    s.place("Jumper:SolderJumper_3_Bridged12", "JP2", "U10_OE_SEL", 200.0, 100.0,
            pin_nets={"1": "OE_BUS", "2": "U10_OE", "3": "OE_FW"}, footprint=SJ, in_bom="no")
    s.place("Jumper:SolderJumper_3_Bridged12", "JP3", "P34_SEL", 100.0, 60.0,
            pin_nets={"1": "AUDIO_PWM", "2": "PICO_P34", "3": "E_BUF"}, footprint=SJ, in_bom="no")

    # ---------- JP4: Q -> /CART autostart tie (open; the classic Program Pak trick) ----------
    s.place("Jumper:SolderJumper_2_Open", "JP4", "CART_TIE", 100.0, 80.0,
            pin_nets={"1": "Q_CART", "2": "CART_CART"},
            footprint="Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", in_bom="no")
    # ---------- /HALT drive (Q2), /NMI and /CART drives (Q3, Q4: DNP) ----------
    res("R1", "4.7k", 30.0, 280.0, "+5V", "HALT_CART")
    res("R2", "4.7k", 40.0, 280.0, "+5V", "NMI_CART", dnp=True)
    res("R3", "4.7k", 50.0, 280.0, "+5V", "RESET_CART")  # populated: bench use without a CoCo
    res("R7", "10k", 40.0, 160.0, "+3V3", "GATE_Q2")  # 10k beats the RP2350 reset pull-down (100k did not)
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

    # ---------- Reset, 3V3_EN, series R ----------
    res("R4", "10k", 80.0, 280.0, "VSYS_PICO", "PICO_3V3_EN")
    res("R9", "100", 200.0, 260.0, "RESET_BUF", "PICO_RUN")
    res("R10", "10k", 210.0, 260.0, "+3V3", "PICO_RUN", dnp=True)
    res("R23", "10k", 220.0, 260.0, "+5V", "SLENB_CART")    # pin 40 is cart->CoCo; nothing else drives U13 A5
    res("R25", "10k", 230.0, 260.0, "+3V3", "U10_OE")  # populated: an unprogrammed module with JP2 at 2-3 must not enable U10
    res("R11", "33", 210.0, 90.0, "OE_BUS_RAW", "OE_BUS")
    res("R12", "33", 140.0, 230.0, "RW_BUF_RAW", "RW_BUF")

    # ---------- Sound stage: AUDIO_PWM -> 2-pole RC -> divider -> SND_CART ----------
    res("R19", "1k", 120.0, 300.0, "AUDIO_PWM", "AUDIO_F1")
    cap("C13", "10nF", 125.0, 310.0, "AUDIO_F1", "GND")
    res("R20", "1k", 130.0, 300.0, "AUDIO_F1", "AUDIO_F2")
    cap("C14", "10nF", 135.0, 310.0, "AUDIO_F2", "GND")
    res("R21", "2.2k", 140.0, 300.0, "AUDIO_AC", "SND_CART")
    res("R22", "1k", 150.0, 310.0, "SND_CART", "GND")
    cap("C15", "1uF", 145.0, 290.0, "AUDIO_F2", "AUDIO_AC", dnp=True)   # AC-coupling option: fit C15, remove R24
    res("R24", "0", 145.0, 300.0, "AUDIO_F2", "AUDIO_AC")            # DC-coupled default (bypasses C15)

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

    # ---------- Test points, fiducials ----------
    for ref, net, tx, ty in (("TP1", "OE_BUS", 320.0, 60.0), ("TP2", "RW_BUF", 320.0, 70.0),
                             ("TP3", "CTS_BUF", 320.0, 80.0), ("TP4", "SCS_BUF", 320.0, 90.0),
                             ("TP5", "E_BUF", 310.0, 60.0), ("TP6", "+3V3", 310.0, 70.0),
                             ("TP7", "SND_CART", 310.0, 80.0), ("TP8", "GND", 310.0, 90.0)):
        s.place("Connector:TestPoint", ref, net, tx, ty, pin_nets={"1": net},
                footprint="TestPoint:TestPoint_Pad_1.0x1.0mm", in_bom="no")
    for ref, fx in (("FID1", 330.0), ("FID2", 340.0), ("FID3", 350.0)):
        s.place("Mechanical:Fiducial", ref, "FID", fx, 30.0, footprint="Fiducial:Fiducial_1mm_Mask2mm", in_bom="no")

    # ---------- PWR_FLAG on VSYS_PICO (D2 cathode is passive; ERC needs a driver) ----------
    s.powers.append(power_port("power:PWR_FLAG", 250.0, 60.0, 0))
    s.labels.append(global_label("VSYS_PICO", 250.0, 60.0, 0, "left", "bidirectional"))

    title_block = (
        '  (title_block\n'
        '    (title "PiCoCo - Pi Pico 2 to Tandy CoCo Cartridge")\n'
        '    (date "2026-09-18")\n'
        '    (rev "2.3.1")\n'
        '    (company "Nathan Byrd")\n'
        '    (comment 1 "4x LVC245A at 3.3 V (U10 data bidi, U11-U13 in), 74LVC00 NAND-NAND /OE = (CTS|SCS) & E")\n'
        '    (comment 2 "GPIO: GP0-7=D0-D7, GP8-21=A0-A13, GP22=/R/W, hdr31=OE_BUS, hdr32=HALT_GATE, hdr34=audio (JP3 2-3: E); '
        'Plus-W pads GP24-30 capture, GP31 FW /OE, GP32/33 NMI/CART drive, GP34 audio; '
        'no SWD header: use the module\'s own debug pads")\n'
        '    (comment 3 "MVP: HDB-DOS ROM over /CTS + Becker $FF41/$FF42 over /SCS; firmware disambiguates by A13")\n'
        '    (comment 4 "Spec: docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md")\n'
        '  )'
    )
    return assemble(lib_block, s, "A2", title_block), s


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
    assert jp3["1"] == "AUDIO_PWM" and jp3["2"] == "PICO_P34" and jp3["3"] == "E_BUF", "JP3 wiring (audio default)"
    for c in s.placed:
        if c["in_bom"] == "yes" and not c["dnp"] and c["ref"] not in ("U15", "C12"):
            assert c["lcsc"], f'{c["ref"]} has no LCSC number'
    print(f"check ok: {len(s.placed)} symbols, {len(nets)} nets")


def assemble(lib_block: str, s: "Sheet", paper: str, title_block: str) -> str:
    """Wrap a populated Sheet into a complete .kicad_sch document."""
    header = (
        '(kicad_sch (version 20230121) (generator gen_schematic_py)\n'
        f'  (uuid {SHEET_UUID})\n'
        f'  (paper "{paper}")\n'
        f'{title_block}\n'
    )
    body = "\n".join(
        [lib_block]
        + s.labels
        + s.no_connects
        + s.components
        + s.powers
    )
    footer = (
        '  (sheet_instances\n'
        '    (path "/" (page "1"))\n'
        '  )\n'
        ')\n'
    )
    return header + body + "\n" + footer


def main() -> None:
    write_library_items()
    text, sheet = build()
    check(sheet)
    SCH_OUT.write_text(text)
    print(f"wrote {SCH_OUT}  ({len(text):,} bytes)")


if __name__ == "__main__":
    main()
