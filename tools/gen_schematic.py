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
    ("PiCoCo:Pico", LOCAL_SYMS, "Pico"),
    ("PiCoCo:COCO-CART", LOCAL_SYMS, "COCO-CART"),
    ("Logic_LevelTranslator:SN74LVC8T245",
     KICAD_STOCK / "Logic_LevelTranslator.kicad_sym", "SN74LVC8T245"),
    # 74LS245 used as the symbol (pin-compatible with 74LVC245A);
    # actual part set via Value = "SN74LVC245AD".
    ("74xx:74LS245", KICAD_STOCK / "74xx.kicad_sym", "74LS245"),
    # U15 is now 74LVC1G11 (3-input AND) so /OE can be gated by E as well
    # as /CTS AND /SCS. Pinout: 1=IN_A, 2=GND, 3=IN_B, 4=OUT, 5=VCC, 6=IN_C.
    ("74xGxx:74LVC1G11",
     KICAD_STOCK / "74xGxx.kicad_sym", "74LVC1G11"),
    # AP1117-15 used as the symbol (pin-compatible with AMS1117);
    # actual part set via Value = "AMS1117-3.3".
    ("Regulator_Linear:AP1117-15",
     KICAD_STOCK / "Regulator_Linear.kicad_sym", "AP1117-15"),
    # 2N7002 extends Q_NMOS_GSD; the parent has the pin definitions.
    # Pins: 1=G, 2=S, 3=D. Value set to "2N7002" at placement time.
    ("Transistor_FET:Q_NMOS_GSD",
     KICAD_STOCK / "Transistor_FET.kicad_sym", "Q_NMOS_GSD"),
    ("Device:R", KICAD_STOCK / "Device.kicad_sym", "R"),
    ("Device:C", KICAD_STOCK / "Device.kicad_sym", "C"),
    # Generic Schottky (value = "SS14" set at placement).
    ("Device:D_Schottky", KICAD_STOCK / "Device.kicad_sym", "D_Schottky"),
    ("Connector_Generic:Conn_01x04",
     KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_01x04"),
    ("Connector_Generic:Conn_01x02",
     KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_01x02"),
    ("Connector:TestPoint",
     KICAD_STOCK / "Connector.kicad_sym", "TestPoint"),
    ("power:+5V", KICAD_STOCK / "power.kicad_sym", "+5V"),
    ("power:+3V3", KICAD_STOCK / "power.kicad_sym", "+3V3"),
    ("power:GND", KICAD_STOCK / "power.kicad_sym", "GND"),
    ("power:PWR_FLAG", KICAD_STOCK / "power.kicad_sym", "PWR_FLAG"),
]


def load_symbols(symbols=None) -> tuple[str, dict[str, dict]]:
    """Return (embedded_block_text, pin_map). pin_map: libid -> {num: pin}."""
    blocks = []
    pin_map: dict[str, dict] = {}
    for libid, path, name in (symbols or SYMBOLS):
        block = extract_symbol(path, name)
        pins = parse_pins(block)
        # Note: pin_map key is the short symbol name as found in the source,
        # but we also index by libid for convenience.
        pin_map[libid] = {p["num"]: p for p in pins}
        block = rewrite_symbol_libid(block, libid)
        blocks.append(block)
    body = "\n".join(blocks)
    return f"  (lib_symbols\n{indent(body, '  ')}\n  )", pin_map


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
    return (
        f'  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} {rot}) {mir}(unit {unit})\n'
        f'    (in_bom {in_bom}) (on_board {on_board}) (dnp no)\n'
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

    def place(
        self, lib_id: str, ref: str, value: str, x: float, y: float,
        rot: int = 0, mirror: str = "",
        pin_nets: dict[str, str] | None = None,
        footprint: str = "",
        power_port_nets: dict[str, str] | None = None,
    ) -> None:
        """Place a component and wire its pins via global labels.

        pin_nets: {pin_number: net_name}. Any pin not in this dict gets a
        `no_connect` marker. Pin "name" strings that are "+5V", "+3V3",
        or "GND" get a power-port instead of a global label (cleaner look).
        """
        self.components.append(
            symbol_instance(lib_id, ref, value, x, y, rot, mirror, footprint)
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

def build() -> str:
    lib_block, pin_map = load_symbols()
    s = Sheet(pin_map)

    # ---------- U1: Pi Pico 2 ----------
    pico_x, pico_y = 80.0, 120.0
    pico_nets = {
        # Left column
        "1":  "D0",   "2":  "D1",  "3":  "GND",  "4":  "D2",
        "5":  "D3",   "6":  "D4",  "7":  "D5",  "8":  "GND",
        "9":  "D6",   "10": "D7",  "11": "A0_B", "12": "A1_B",
        "13": "GND",  "14": "A2_B","15": "A3_B", "16": "A4_B",
        "17": "A5_B", "18": "GND","19": "A6_B", "20": "A7_B",
        # Right column
        "21": "A8_B", "22": "A9_B","23": "GND", "24": "A10_B",
        "25": "A11_B","26": "A12_B","27": "A13_B","28": "GND",
        # GP22 = /R/W, connects to U12 buffered output RW_BUF.
        "29": "RW_BUF",
        # Pin 30 (RUN) is now driven by CoCo /RESET via U13's RESET_DBG,
        # through R9 (100 ohm series) with R10 (10k) pulling up to +3V3
        # so the Pico sees a clean logic-high when CoCo is not in reset.
        "30": "PICO_RUN",
        # GP26 = OE_BUS (single cart-selected input from U15); firmware
        # disambiguates /CTS vs /SCS by A13 (=0 => ROM, =1 => Becker).
        "31": "OE_BUS",
        # GP27 = HALT_GATE: firmware output driving Q2 gate via R8.
        # Holds /HALT low at boot until PIO is armed, then releases.
        "32": "HALT_GATE",
        "33": "GND",
        "34": "E_B",
        # 35 VREF - NC
        # 36 3V3 - NC (do not back-feed)
        "37": "PICO_3V3_EN",
        "38": "GND",
        # VSYS is now fed from +5V via Schottky D2 (net VSYS_PICO);
        # 3V3_EN is pulled up to the same VSYS_PICO rail, not +3V3,
        # to avoid latch-up when +3V3 is the thing the buck generates.
        "39": "VSYS_PICO",
        # 40 VBUS - NC
        # SWCLK/SWDIO each go through a 100-ohm series (R13/R14) to
        # J_SWD, protecting the Pico from probe-driven transients.
        "41": "SWCLK_PICO","42": "GND","43": "SWDIO_PICO",
    }
    s.place("PiCoCo:Pico", "U1", "Pico 2",
            pico_x, pico_y, pin_nets=pico_nets,
            footprint="PiCoCo:RPi_Pico_SMD_TH")

    # ---------- P1: Cartridge edge ----------
    cart_x, cart_y = 260.0, 140.0
    cart_nets = {
        # 1, 2 +/-12V not used
        "3":  "HALT_CART",
        "4":  "NMI_CART",
        "5":  "RESET_CART",
        "6":  "E_CART",
        "7":  "Q_CART",
        "8":  "CART_CART",
        "9":  "+5V",
        "10": "D0_CART", "11": "D1_CART", "12": "D2_CART", "13": "D3_CART",
        "14": "D4_CART", "15": "D5_CART", "16": "D6_CART", "17": "D7_CART",
        "18": "RW_CART",
        "19": "A0_CART", "20": "A1_CART", "21": "A2_CART", "22": "A3_CART",
        "23": "A4_CART", "24": "A5_CART", "25": "A6_CART", "26": "A7_CART",
        "27": "A8_CART", "28": "A9_CART", "29": "A10_CART","30": "A11_CART",
        "31": "A12_CART",
        "32": "CTS_CART",
        "33": "GND", "34": "GND",
        # 35 SND not used
        "36": "SCS_CART",
        "37": "A13_CART",
        "38": "A14_CART",
        "39": "A15_CART",
        "40": "SLENB_CART",
    }
    s.place("PiCoCo:COCO-CART", "P1", "COCO-CART",
            cart_x, cart_y, pin_nets=cart_nets,
            footprint="PiCoCo:COCO-CART-2.1X1.75")

    # ---------- U10: SN74LVC8T245 (D0-D7 bidi) ----------
    u10_x, u10_y = 170.0, 130.0
    # Actual KiCad symbol pinout:
    # 1=Vcca(3V3), 2=DIR, 3..10=A1..A8 (cart D0..D7),
    # 11..13=GND, 14..21=B8..B1 (Pico D7..D0),
    # 22=/OE, 23..24=Vccb(5V)
    u10_nets = {
        "1":  "+3V3",        # Vcca (Pico side rail)
        "2":  "RW_BUF",      # DIR <- buffered /R/W
        "3":  "D0_CART",     # A1
        "4":  "D1_CART",
        "5":  "D2_CART",
        "6":  "D3_CART",
        "7":  "D4_CART",
        "8":  "D5_CART",
        "9":  "D6_CART",
        "10": "D7_CART",     # A8
        "11": "GND",
        "12": "GND",
        "13": "GND",
        "14": "D7",          # B8
        "15": "D6",
        "16": "D5",
        "17": "D4",
        "18": "D3",
        "19": "D2",
        "20": "D1",
        "21": "D0",          # B1
        "22": "OE_BUS",      # /OE from U15
        "23": "+5V",         # Vccb (CoCo side rail)
        "24": "+5V",
    }
    s.place("Logic_LevelTranslator:SN74LVC8T245", "U10", "SN74LVC8T245DW",
            u10_x, u10_y, pin_nets=u10_nets,
            footprint="Package_SO:SOIC-24W_7.5x15.4mm_P1.27mm")

    # ---------- U11: 74LVC245A (A0-A7 buffer) ----------
    # 74LS245 symbol pinout: 1=DIR (A->B), 2..9=A0..A7, 10=GND,
    # 11..18=B7..B0 (reverse order!), 19=/OE, 20=VCC
    u11_x, u11_y = 80.0, 220.0
    u11_nets = {
        "1":  "+3V3",        # DIR high = A->B
        "2":  "A0_CART",  "3":  "A1_CART",  "4":  "A2_CART",  "5":  "A3_CART",
        "6":  "A4_CART",  "7":  "A5_CART",  "8":  "A6_CART",  "9":  "A7_CART",
        "10": "GND",
        "11": "A7_B",  "12": "A6_B",  "13": "A5_B",  "14": "A4_B",
        "15": "A3_B",  "16": "A2_B",  "17": "A1_B",  "18": "A0_B",
        "19": "GND",         # /OE tied low
        "20": "+3V3",        # VCC
    }
    s.place("74xx:74LS245", "U11", "SN74LVC245AD",
            u11_x, u11_y, pin_nets=u11_nets,
            footprint="Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm")

    # ---------- U12: 74LVC245A (A8-A13 + R/W + CTS + SCS) ----------
    u12_x, u12_y = 130.0, 220.0
    u12_nets = {
        "1":  "+3V3",
        "2":  "A8_CART",  "3":  "A9_CART",  "4":  "A10_CART", "5":  "A11_CART",
        "6":  "A12_CART", "7":  "A13_CART", "8":  "RW_CART",  "9":  "CTS_CART",
        "10": "GND",
        # B-side reversed: B7, B6, B5, B4, B3, B2, B1, B0
        # RW_BUF_RAW is the U12 output; R12 terminates it to RW_BUF which
        # fans out to U10 DIR, Pico GP22, and J_LVC.
        "11": "CTS_BUF", "12": "RW_BUF_RAW", "13": "A13_B", "14": "A12_B",
        "15": "A11_B",   "16": "A10_B",  "17": "A9_B",  "18": "A8_B",
        "19": "GND",
        "20": "+3V3",
    }
    s.place("74xx:74LS245", "U12", "SN74LVC245AD",
            u12_x, u12_y, pin_nets=u12_nets,
            footprint="Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm")

    # ---------- U13: 74LVC245A (SCS, E, Q, SLENB, HALT, NMI, RESET, CART) ----------
    u13_x, u13_y = 180.0, 220.0
    # U13 now only buffers signals that have real consumers:
    # /SCS → U15, E → U15 and Pico GP28, /RESET → Pico RUN (via R9).
    # Q, /SLENB, /HALT, /NMI, /CART inputs are still tied so the
    # buffer loads the CoCo side correctly, but their outputs are
    # left unconnected (pins not in the dict emit no_connect).
    # If you ever want debug taps on these signals, add test points
    # on the corresponding U13 output pin(s).
    u13_nets = {
        "1":  "+3V3",
        "2":  "SCS_CART",   "3":  "E_CART",     "4":  "Q_CART",
        "5":  "SLENB_CART", "6":  "HALT_CART",  "7":  "NMI_CART",
        "8":  "RESET_CART", "9":  "CART_CART",
        "10": "GND",
        # 11: CART_DBG output — no_connect
        "12": "RESET_DBG",  # drives Pico RUN via R9+R10
        # 13: NMI_DBG output — no_connect
        # 14: HALT_DBG output — no_connect
        # 15: SLENB_DBG output — no_connect
        # 16: Q_DBG output — no_connect
        "17": "E_B",        # drives Pico GP28 and U15 IN_C
        "18": "SCS_BUF",    # drives U15 IN_B
        "19": "GND",
        "20": "+3V3",
    }
    s.place("74xx:74LS245", "U13", "SN74LVC245AD",
            u13_x, u13_y, pin_nets=u13_nets,
            footprint="Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm")

    # ---------- U14: AMS1117-3.3 (AP1117 symbol, pin-compatible) ----------
    u14_x, u14_y = 260.0, 50.0
    # AP1117-15 pins: 1=GND, 2=VO (3V3), 3=VI (5V)
    u14_nets = {
        "1": "GND",
        "2": "+3V3",
        "3": "+5V",
    }
    s.place("Regulator_Linear:AP1117-15", "U14", "AMS1117-3.3",
            u14_x, u14_y, pin_nets=u14_nets,
            footprint="Package_TO_SOT_SMD:SOT-223-3_TabPin2")

    # ---------- U15: 74LVC1G11 (3-input AND gate) ----------
    # /OE = /CTS AND /SCS AND /E -- qualified by E so U10 is only enabled
    # during the valid data phase, not during the address-setup phase.
    # Both /CTS and /SCS idle high; E is high during the data phase of a
    # cycle. The combined high-when-all-asserted logic drops /OE (active
    # low on U10) whenever the cart is selected AND we're in the E-high
    # data window.
    u15_x, u15_y = 220.0, 80.0
    # KiCad pinout for 74LVC1G11 (SOT-363/SC-70-6):
    # 1=IN_A, 2=GND, 3=IN_B, 4=OUT, 5=VCC, 6=IN_C
    u15_nets = {
        "1": "CTS_BUF",   # IN_A
        "2": "GND",
        "3": "SCS_BUF",   # IN_B
        "4": "OE_BUS_RAW",  # OUT (pre-termination; goes through R11)
        "5": "+3V3",      # VCC
        "6": "E_B",       # IN_C (new: qualifies by E)
    }
    s.place("74xGxx:74LVC1G11", "U15", "SN74LVC1G11",
            u15_x, u15_y, pin_nets=u15_nets,
            footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6")

    # ---------- Passives: pull-ups + series R + 3V3_EN R ----------
    # Resistors have pins 1 and 2 in the symbol
    # R1: HALT_CART pull-up to +5V
    for i, (ref, net) in enumerate([
        ("R1", "HALT_CART"),
        ("R2", "NMI_CART"),
        ("R3", "RESET_CART"),
    ]):
        rx, ry = 30.0 + i * 10.0, 280.0
        s.place("Device:R", ref, "4.7k",
                rx, ry,
                pin_nets={"1": "+5V", "2": net},
                footprint="Resistor_SMD:R_0805_2012Metric")

    # R4: 3V3_EN pull-up to VSYS_PICO (not +3V3) so the Pico's internal
    # buck-boost enable doesn't depend on the rail it's generating. This
    # avoids the brown-out latch where a dipping +3V3 pulls 3V3_EN low,
    # shutting off the buck, which keeps +3V3 low, etc.
    s.place("Device:R", "R4", "10k",
            80.0, 280.0,
            pin_nets={"1": "VSYS_PICO", "2": "PICO_3V3_EN"},
            footprint="Resistor_SMD:R_0805_2012Metric")

    # ---------- /CART pull-up with jumper (R6 + JP1) ----------
    # CoCo pin 8 (/CART) should idle at +5V to signal cartridge presence.
    # R6 is the pull-up; JP1 lets the user disable it (remove shunt) in
    # case a multipak or special configuration drives /CART externally.
    s.place("Device:R", "R6", "4.7k",
            310.0, 100.0,
            pin_nets={"1": "+5V", "2": "CART_PU"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    s.place("Connector_Generic:Conn_01x02", "JP1", "CART_EN",
            320.0, 100.0,
            pin_nets={"1": "CART_PU", "2": "CART_CART"},
            footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical")

    # ---------- /HALT firmware-controlled drive (R7 + R8 + Q2) ----------
    # Q2 pulls /HALT_CART low when its gate is high. At Pico boot GP27 is
    # high-Z so R7 (to +3V3) turns Q2 on, asserting /HALT and holding the
    # CoCo CPU until firmware is ready. Firmware then drives GP27 LOW to
    # pull the gate down (through R8's 100-ohm) and release /HALT.
    # Firmware can re-assert /HALT later by driving GP27 HIGH (e.g., for
    # DriveWire flow control of long host-side operations).
    s.place("Device:R", "R7", "100k",
            40.0, 160.0,
            pin_nets={"1": "+3V3", "2": "GATE_Q2"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    s.place("Device:R", "R8", "100",
            50.0, 160.0,
            pin_nets={"1": "HALT_GATE", "2": "GATE_Q2"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    # Q_NMOS_GSD (parent of 2N7002): pin 1=G, 2=S, 3=D.
    s.place("Transistor_FET:Q_NMOS_GSD", "Q2", "2N7002",
            60.0, 170.0,
            pin_nets={"1": "GATE_Q2", "2": "GND", "3": "HALT_CART"},
            footprint="Package_TO_SOT_SMD:SOT-23")

    # ---------- Pico RUN from CoCo /RESET (R9 + R10) ----------
    # RESET_DBG is the 3V3-level buffered /RESET out of U13 ch7. Series
    # R9 protects the Pico from transients; R10 pulls RUN high so the
    # Pico operates normally when /RESET is deasserted.
    s.place("Device:R", "R9", "100",
            200.0, 260.0,
            pin_nets={"1": "RESET_DBG", "2": "PICO_RUN"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    s.place("Device:R", "R10", "10k",
            210.0, 260.0,
            pin_nets={"1": "+3V3", "2": "PICO_RUN"},
            footprint="Resistor_SMD:R_0805_2012Metric")

    # ---------- Series termination (R11 + R12) ----------
    # OE_BUS fans out to U10 /OE, Pico GP26, and J_LVC; RW_BUF to U10
    # DIR, Pico GP22, and J_LVC. 33-ohm near each source damps reflections
    # on LVC edges into the 3-load nets.
    s.place("Device:R", "R11", "33",
            210.0, 90.0,
            pin_nets={"1": "OE_BUS_RAW", "2": "OE_BUS"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    s.place("Device:R", "R12", "33",
            140.0, 230.0,
            pin_nets={"1": "RW_BUF_RAW", "2": "RW_BUF"},
            footprint="Resistor_SMD:R_0805_2012Metric")

    # ---------- SWD series protection (R13 + R14) ----------
    # Prevents probe-driven transients on SWCLK/SWDIO from reaching Pico
    # pads directly. Also keeps J_SWD net names as SWCLK/SWDIO for
    # external compatibility.
    s.place("Device:R", "R13", "100",
            30.0, 225.0,
            pin_nets={"1": "SWCLK_PICO", "2": "SWCLK"},
            footprint="Resistor_SMD:R_0805_2012Metric")
    s.place("Device:R", "R14", "100",
            30.0, 235.0,
            pin_nets={"1": "SWDIO_PICO", "2": "SWDIO"},
            footprint="Resistor_SMD:R_0805_2012Metric")

    # ---------- D2: Schottky from +5V to Pico VSYS ----------
    # Drops ~0.3V so VSYS sees ~4.7V, safely within Pico 2 buck-boost
    # input range (1.8-5.5V). Fixes the previous cascade where VSYS was
    # fed from the AMS1117 output (inefficient, brown-out latch risk).
    # D_Schottky pins: 1=K (cathode), 2=A (anode).
    s.place("Device:D_Schottky", "D2", "SS14",
            250.0, 70.0,
            pin_nets={"1": "VSYS_PICO", "2": "+5V"},
            footprint="Diode_SMD:D_SMA")

    # ---------- Capacitors ----------
    # C1: +5V bulk at cart edge
    s.place("Device:C", "C1", "10uF",
            300.0, 200.0,
            pin_nets={"1": "+5V", "2": "GND"},
            footprint="Capacitor_SMD:C_0805_2012Metric")

    # C2: LDO input
    s.place("Device:C", "C2", "10uF",
            240.0, 30.0,
            pin_nets={"1": "+5V", "2": "GND"},
            footprint="Capacitor_SMD:C_0805_2012Metric")

    # C3: LDO output
    s.place("Device:C", "C3", "22uF",
            280.0, 30.0,
            pin_nets={"1": "+3V3", "2": "GND"},
            footprint="Capacitor_SMD:C_0805_2012Metric")

    # C4..C10: Per-IC decoupling
    decoup_positions = [
        ("C4", 160.0, 100.0, "+3V3"),   # U10 Vcca
        ("C5", 160.0, 110.0, "+5V"),    # U10 Vccb
        ("C6",  90.0, 260.0, "+3V3"),   # U11
        ("C7", 140.0, 260.0, "+3V3"),   # U12
        ("C8", 190.0, 260.0, "+3V3"),   # U13
        ("C9", 230.0,  70.0, "+3V3"),   # U15
        ("C10",100.0, 100.0, "+3V3"),   # Pico local
    ]
    for ref, cx, cy, rail in decoup_positions:
        s.place("Device:C", ref, "100nF",
                cx, cy,
                pin_nets={"1": rail, "2": "GND"},
                footprint="Capacitor_SMD:C_0805_2012Metric")

    # C11: local 10uF bulk at U10 Vccb to handle data-bus switching
    # transients. Supplements C1 (edge-connector bulk) which is too far
    # from U10 to respond to 1.79 MHz D0-D7 edges.
    s.place("Device:C", "C11", "10uF",
            155.0, 115.0,
            pin_nets={"1": "+5V", "2": "GND"},
            footprint="Capacitor_SMD:C_0805_2012Metric")

    # ---------- Test points ----------
    # TP1-TP6 are 1.0x1.0 mm SMD pads for oscilloscope/logic-analyzer
    # access to the signals most needed during bring-up. Connector:
    # TestPoint has a single pin (1) whose net is the probe signal.
    test_points = [
        ("TP1", "OE_BUS",   320.0, 60.0),
        ("TP2", "RW_BUF",   320.0, 70.0),
        ("TP3", "CTS_BUF",  320.0, 80.0),
        ("TP4", "SCS_BUF",  320.0, 90.0),
        ("TP5", "E_B",      310.0, 60.0),
        ("TP6", "+3V3",     310.0, 70.0),
    ]
    for ref, net, tx, ty in test_points:
        s.place("Connector:TestPoint", ref, net,
                tx, ty,
                pin_nets={"1": net},
                footprint="TestPoint:TestPoint_Pad_1.0x1.0mm")

    # ---------- Debug / programming header ----------
    # J_CART and J_LVC 2x20 debug breakouts were removed in v2.2 — they
    # consumed too much board area on the 98x55 mm card. Bring-up
    # debugging now happens via:
    #   - TP1..TP6 SMD pads on key nets (see §7 of hardware-design.md)
    #   - the Pico's USB CDC trace stream (firmware §10)
    #   - J_SWD 4-pin header for SWD access during development
    # If you ever need deeper probing on individual cart signals, add
    # TP pads to the relevant U13 output pin(s); the buffers still run.

    # J_SWD: 1x4 header (SWCLK, GND, SWDIO, +3V3)
    s.place("Connector_Generic:Conn_01x04", "J_SWD", "SWD",
            20.0, 230.0,
            pin_nets={"1": "SWCLK", "2": "GND", "3": "SWDIO", "4": "+3V3"},
            footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical")

    # ---------- PWR_FLAGs: not needed ----------
    # +5V and GND are driven by P1 (COCO-CART symbol declares pins 9,
    # 33, 34 as power_output). +3V3 is driven by U14 pin 2 (VO,
    # power_output). So every power net has a real power_output driver
    # and no PWR_FLAGs are necessary.

    title_block = (
        '  (title_block\n'
        '    (title "PiCoCo - Pi Pico 2 to Tandy CoCo Cartridge")\n'
        '    (date "2026-04-19")\n'
        '    (rev "2.2")\n'
        '    (company "Nathan Byrd")\n'
        '    (comment 1 "LVC8T245 data bus + 3x LVC245A controls + 1G11 3-input /OE gate (E AND /CTS AND /SCS)")\n'
        '    (comment 2 "GPIO: GP0-7=D0-D7, GP8-21=A0-A13, GP22=/R/W, GP26=OE_BUS, GP27=HALT_GATE, GP28=E; RUN from CoCo /RESET")\n'
        '    (comment 3 "MVP: HDB-DOS ROM over /CTS + Becker $FF41/$FF42 over /SCS; firmware disambiguates by A13")\n'
        '    (comment 4 "See docs/hardware-design.md and docs/firmware-architecture.md")\n'
        '  )'
    )
    return assemble(lib_block, s, "A2", title_block)


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
    text = build()
    SCH_OUT.write_text(text)
    print(f"wrote {SCH_OUT}  ({len(text):,} bytes)")


if __name__ == "__main__":
    main()
