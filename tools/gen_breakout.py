#!/usr/bin/env python3
"""
Generate the PiCoCo cartridge-edge breakout board: breakout/PiCoCo-Breakout.*

Run with KiCad's bundled Python (it needs the pcbnew module):

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tools/gen_breakout.py

What it writes:
  libraries/PiCoCo.pretty/COCO-CART-FINGERS.kicad_mod   fingers-only footprint (derived)
  libraries/PiCoCo.kicad_sym                            adds/refreshes COCO-CART-FINGERS symbol
  breakout/PiCoCo-Breakout.kicad_sch                    netlist-grade schematic (label-at-pin)
  breakout/PiCoCo-Breakout.kicad_pcb                    fully routed + zoned PCB
  breakout/PiCoCo-Breakout.kicad_pro, fp-lib-table, sym-lib-table
  breakout/PiCoCo-Cobbler.{kicad_sch,kicad_pcb,kicad_pro}   keyed 2x20 IDC -> breadboard, 1:1

The Cobbler exists because Raspberry Pi cobblers bus their GND/5V pins
together on the adapter PCB (verified in Adafruit's T-Cobbler Plus Eagle
schematic), which would short CoCo bus lines through the ribbon.

Board: 40 gold fingers -> keyed 2x20 IDC header, pin n = finger n, with a
polyfuse on +5V, bulk + bypass caps, and a power LED. Fingers 1 and 2
(+/-12 V on CoCo 1/2) are deliberately left open; header pins 1 and 2
are tied to GND instead, giving the ribbon two extra return conductors.

Routing scheme (all in footprint-local mm, y negative = up the board):
  odd fingers  (F.Cu pads) -> straight up on F.Cu into the near header row
  even fingers (B.Cu pads) -> up on B.Cu, jog into the lane between two
                              near-row pins, into the far header row
  finger 9 (+5V) detours through F1 in the component strip above the header
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_schematic as g  # noqa: E402

try:
    import pcbnew  # type: ignore
except ImportError:
    sys.exit("pcbnew module not found. Run this with KiCad's bundled python3 "
             "(see docstring).")

ROOT = g.PROJECT_ROOT
OUT = ROOT / "breakout"
NAME = "PiCoCo-Breakout"
PRETTY = ROOT / "libraries" / "PiCoCo.pretty"
STOCK_FP = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")

g.PROJECT_NAME = NAME

# ---------------------------------------------------------------------------
# Signal table: finger/header pin -> (net, silk label). None = not routed.
# ---------------------------------------------------------------------------
PINS: dict[int, tuple[str | None, str]] = {
    1: (None, "1:GND"), 2: (None, "2:GND"),   # cart fingers open; header pins tied to GND
    3: ("HALT", "/HALT"), 4: ("NMI", "/NMI"), 5: ("RESET", "/RESET"),
    6: ("E", "E"), 7: ("Q", "Q"), 8: ("CART", "/CART"),
    9: ("+5V", "+5V"),
    **{10 + i: (f"D{i}", f"D{i}") for i in range(8)},
    18: ("RW", "R/W"),
    **{19 + i: (f"A{i}", f"A{i}") for i in range(8)},
    **{27 + i: (f"A{8 + i}", f"A{8 + i}") for i in range(5)},
    32: ("CTS", "/CTS"), 33: ("GND", "GND"), 34: ("GND", "GND"),
    35: ("SND", "SND"), 36: ("SCS", "/SCS"), 37: ("A13", "A13"),
    38: ("A14", "A14"), 39: ("A15", "A15"), 40: ("SLENB", "/SLENB"),
}
CART_PINS = {n: ("+5V_CART" if n == 9 else net) for n, (net, _) in PINS.items() if net}
HDR_PINS = {**{n: net for n, (net, _) in PINS.items() if net}, 1: "GND", 2: "GND"}

# ---------------------------------------------------------------------------
# Derived library parts (fingers only: no mounting hole, no Pak outline)
# ---------------------------------------------------------------------------

def _top_level_items(body: str) -> list[str]:
    """Split the inside of an s-expr node into its balanced child nodes."""
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


def write_fingers_footprint() -> None:
    src = (PRETTY / "COCO-CART-2.1X1.75.kicad_mod").read_text()
    head_end = src.index("\n", src.index("(footprint"))
    body = src[head_end:src.rstrip().rfind(")")]
    keep = []
    for it in _top_level_items(body):
        kind = it.split(None, 2)[0].lstrip("(")
        if kind in ("fp_line", "fp_arc", "zone"):
            continue                     # Pak outline + broken keepouts
        if kind == "pad" and '"MTG1"' in it[:20]:
            continue
        if kind == "descr":
            it = ('(descr "Color Computer cartridge edge fingers only (40 x 0.1in, 2.1in wide). '
                  'Board outline is drawn in the PCB, not the footprint.")')
        # Fab bevel rule: JLCPCB needs >= 0.6 mm and PCBWay >= 0.5 mm from finger to
        # edge for a 30 deg bevel on 1.6 mm stock; the source footprint has 0.44 mm.
        # Pull the leading edge back to 0.80 mm. Width, pitch, and the far end stay.
        it = it.replace("(at 2.54 -5.207) (size 1.27 9.525)", "XX")  # placeholder guard
        it = re.sub(r"\(at ([\d.]+) -5\.207\) \(size 1\.27 9\.525\)",
                    r"(at \1 -5.385) (size 1.27 9.17)", it.replace("XX", "(at 2.54 -5.207) (size 1.27 9.525)"))
        keep.append(it.replace("(thickness 0.1016)", "(thickness 0.15)"))
    text = ('(footprint "COCO-CART-FINGERS" (version 20221018) (generator gen_breakout_py)\n'
            '  (layer "F.Cu")\n  ' + "\n  ".join(keep) + "\n)\n")
    (PRETTY / "COCO-CART-FINGERS.kicad_mod").write_text(text)


def write_fingers_symbol() -> None:
    lib = g.LOCAL_SYMS
    src = g.extract_symbol(lib, "COCO-CART")
    sym = re.sub(r'"COCO-CART(_\d+_\d+)?"', r'"COCO-CART-FINGERS\1"', src)
    # drop the MTG1 pin block
    blocks = g._extract_pin_blocks(sym)
    for b in blocks:
        if '(number "MTG1"' in b:
            sym = sym.replace(b, "")
    # GND@2 becomes passive so two power_out GND pins no longer collide in ERC
    sym = re.sub(r'\(pin power_out (line \(at 5\.08 -50\.8 90\))', r'(pin passive \1', sym)
    sym = re.sub(r'\(property "Footprint" "[^"]*"', '(property "Footprint" "PiCoCo:COCO-CART-FINGERS"', sym)
    assert '(number "MTG1"' not in sym and "COCO-CART-FINGERS" in sym
    text = lib.read_text()
    try:
        old = g.extract_symbol(lib, "COCO-CART-FINGERS")
        text = text.replace(old, sym.strip("\n"))
    except RuntimeError:
        end = text.rstrip().rfind(")")
        text = text[:end].rstrip("\n") + "\n" + sym.rstrip("\n") + "\n" + text[end:]
    lib.write_text(text)


# ---------------------------------------------------------------------------
# Schematic
# ---------------------------------------------------------------------------

SYMBOLS = [
    ("PiCoCo:COCO-CART-FINGERS", g.LOCAL_SYMS, "COCO-CART-FINGERS"),
    ("Connector_Generic:Conn_02x20_Odd_Even",
     g.KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_02x20_Odd_Even"),
    ("Device:Polyfuse", g.KICAD_STOCK / "Device.kicad_sym", "Polyfuse"),
    ("Device:C", g.KICAD_STOCK / "Device.kicad_sym", "C"),
    ("Device:R", g.KICAD_STOCK / "Device.kicad_sym", "R"),
    ("Device:LED", g.KICAD_STOCK / "Device.kicad_sym", "LED"),
    ("power:+5V", g.KICAD_STOCK / "power.kicad_sym", "+5V"),
    ("power:GND", g.KICAD_STOCK / "power.kicad_sym", "GND"),
    ("power:PWR_FLAG", g.KICAD_STOCK / "power.kicad_sym", "PWR_FLAG"),
]

FOOTPRINTS = {
    "P1": ("PiCoCo", "COCO-CART-FINGERS"),
    "J1": ("Connector_IDC", "IDC-Header_2x20_P2.54mm_Vertical"),   # keyed; shroud overhangs the edges 2.8 mm
    "F1": ("Fuse", "Fuse_1812_4532Metric"),
    "C1": ("Capacitor_SMD", "C_0805_2012Metric"),
    "C2": ("Capacitor_SMD", "C_0805_2012Metric"),
    "R1": ("Resistor_SMD", "R_0805_2012Metric"),
    "D1": ("LED_SMD", "LED_0805_2012Metric"),
}
VALUES = {"P1": "COCO-CART", "J1": "CART_BUS", "F1": "0.5A", "C1": "10uF",
          "C2": "100nF", "R1": "1k", "D1": "PWR"}


def fpid(ref: str) -> str:
    lib, name = FOOTPRINTS[ref]
    return f"{lib}:{name}"


def build_schematic() -> str:
    lib_block, pin_map = g.load_symbols(SYMBOLS)
    s = g.Sheet(pin_map)
    s.place("PiCoCo:COCO-CART-FINGERS", "P1", VALUES["P1"], 80.0, 120.0,
            pin_nets={str(k): v for k, v in CART_PINS.items()}, footprint=fpid("P1"))
    s.place("Connector_Generic:Conn_02x20_Odd_Even", "J1", VALUES["J1"], 200.0, 120.0,
            pin_nets={str(k): v for k, v in HDR_PINS.items()}, footprint=fpid("J1"))
    s.place("Device:Polyfuse", "F1", VALUES["F1"], 290.0, 60.0,
            pin_nets={"1": "+5V", "2": "+5V_CART"}, footprint=fpid("F1"))
    s.place("Device:C", "C1", VALUES["C1"], 310.0, 60.0,
            pin_nets={"1": "+5V", "2": "GND"}, footprint=fpid("C1"))
    s.place("Device:C", "C2", VALUES["C2"], 330.0, 60.0,
            pin_nets={"1": "+5V", "2": "GND"}, footprint=fpid("C2"))
    s.place("Device:R", "R1", VALUES["R1"], 350.0, 60.0,
            pin_nets={"1": "+5V", "2": "LED_A"}, footprint=fpid("R1"))
    s.place("Device:LED", "D1", VALUES["D1"], 350.0, 90.0, rot=180,
            pin_nets={"1": "GND", "2": "LED_A"}, footprint=fpid("D1"))
    # +5V has no power_output driver once it is behind the fuse.
    s.place("power:PWR_FLAG", "#FLG01", "PWR_FLAG", 290.0, 100.0, pin_nets={"1": "+5V"})
    title = (
        '  (title_block\n'
        '    (title "PiCoCo Cartridge Breakout")\n'
        '    (date "2026-09-05")\n'
        '    (rev "1.0")\n'
        '    (company "Nathan Byrd")\n'
        '    (comment 1 "CoCo cart edge -> keyed 2x20 IDC header, pin n = finger n; fingers 1,2 (+/-12V) open, header 1,2 = GND")\n'
        '    (comment 2 "Mates with a 40-way IDC ribbon + PiCoCo-Cobbler (NOT a Pi cobbler: those bus GND pins)")\n'
        '    (comment 3 "See docs/breadboard-plan.md and breakout/README.md")\n'
        '  )'
    )
    return g.assemble(lib_block, s, "A3", title)


# ---------------------------------------------------------------------------
# PCB
# ---------------------------------------------------------------------------

OX, OY = 100.0, 150.0          # P1 origin in board coordinates
X0, X1 = 0.254, 53.086         # board side edges (match the finger tab)
TOP = -99.0                    # board top edge (keeps the board inside the 100 mm fab price tier)
FINGER_TOP = -10.16            # end of the finger tab
CASE_Y = -55.0                 # approx. case surface: 10.16 mm tab + measured 1 3/4 in (44.5 mm) recess
Y_NEAR = -77.0                 # header near row (odd pins), pin 1 at x = 2.54; ~19 mm outside the case
PITCH = 2.54
HALF = PITCH / 2
TRACE = 0.3
STRIP_Y = -94.0                # component row
RAIL_Y = -97.0                 # +5V rail


def V(x: float, y: float):
    return pcbnew.VECTOR2I_MM(OX + x, OY + y)


class Pcb:
    def __init__(self):
        self.b = pcbnew.BOARD()
        self.nets: dict[str, pcbnew.NETINFO_ITEM] = {}
        self.fps: dict[str, pcbnew.FOOTPRINT] = {}

    def net(self, name: str):
        if name not in self.nets:
            n = pcbnew.NETINFO_ITEM(self.b, name)
            self.b.Add(n)
            self.nets[name] = n
        return self.nets[name]

    def place(self, ref: str, x: float, y: float, rot: float = 0, pads: dict[str, str] | None = None,
              table: dict | None = None, values: dict | None = None, back: bool = False):
        lib, name = (table or FOOTPRINTS)[ref]
        path = PRETTY if lib == "PiCoCo" else STOCK_FP / f"{lib}.pretty"
        fp = pcbnew.FootprintLoad(str(path), name)
        assert fp is not None, f"footprint {lib}:{name} not found"
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue((values or VALUES)[ref])
        self.b.Add(fp)          # Flip() needs a parent board
        if back:
            fp.SetLayerAndFlip(pcbnew.B_Cu)
        fp.SetPosition(V(x, y))
        fp.SetOrientationDegrees(rot)
        for num, net in (pads or {}).items():
            pad = fp.FindPadByNumber(num)
            assert pad is not None, f"{ref} has no pad {num}"
            pad.SetNet(self.net(net))
        self.fps[ref] = fp
        return fp

    def pad_xy(self, ref: str, num: str) -> tuple[float, float]:
        p = self.fps[ref].FindPadByNumber(num).GetPosition()
        return pcbnew.ToMM(p.x) - OX, pcbnew.ToMM(p.y) - OY

    def track(self, net: str, layer, pts: list[tuple[float, float]], width: float = TRACE):
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(V(ax, ay))
            t.SetEnd(V(bx, by))
            t.SetWidth(pcbnew.FromMM(width))
            t.SetLayer(layer)
            t.SetNet(self.net(net))
            self.b.Add(t)

    def edge_line(self, a, b):
        s = pcbnew.PCB_SHAPE(self.b, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(V(*a)); s.SetEnd(V(*b))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcbnew.FromMM(0.1))
        self.b.Add(s)

    def edge_arc(self, a, mid, b):
        s = pcbnew.PCB_SHAPE(self.b, pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(V(*a), V(*mid), V(*b))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcbnew.FromMM(0.1))
        self.b.Add(s)

    def text(self, txt: str, x: float, y: float, layer, size: float = 0.8,
             rot: float = 0, halign=None, thick: float = 0.15):
        t = pcbnew.PCB_TEXT(self.b)
        t.SetText(txt)
        t.SetPosition(V(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
        t.SetTextThickness(pcbnew.FromMM(thick))
        t.SetTextAngleDegrees(rot)
        if halign is not None:
            t.SetHorizJustify(halign)
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        self.b.Add(t)

    def zone(self, layer, net: str, rect):
        (x0, y0), (x1, y1) = rect
        z = pcbnew.ZONE(self.b)
        z.SetLayer(layer)
        z.SetNet(self.net(net))
        z.SetLocalClearance(pcbnew.FromMM(0.25))
        z.SetMinThickness(pcbnew.FromMM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(pcbnew.FromMM(0.3))
        z.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.4))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        o = z.Outline()
        o.NewOutline()
        for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            p = V(x, y)
            o.Append(p.x, p.y)
        self.b.Add(z)


def build_pcb() -> pcbnew.BOARD:
    p = Pcb()
    F, B = pcbnew.F_Cu, pcbnew.B_Cu

    # --- footprints -------------------------------------------------------
    p.place("P1", 0, 0, 0, {**{str(k): v for k, v in CART_PINS.items()},
                            "1": "unconnected-(P1--12V-Pad1)", "2": "unconnected-(P1-+12V-Pad2)"})
    p.fps["P1"].Value().SetVisible(False)
    j1 = p.place("J1", PITCH, Y_NEAR, 90, {str(k): v for k, v in HDR_PINS.items()})
    # The shroud outline lands outside the 52.8 mm board; the shroud itself is the key.
    for item in list(j1.GraphicalItems()):
        if item.GetLayer() == pcbnew.F_SilkS:
            j1.Remove(item)
    p1, p2, p3 = (p.pad_xy("J1", n) for n in ("1", "2", "3"))
    assert abs(p3[0] - p1[0] - PITCH) < 1e-6 and abs(p2[1] - p1[1] + PITCH) < 1e-6, \
        f"unexpected header orientation: {p1} {p2} {p3}"
    y_far = p2[1]
    j1.Reference().SetVisible(False)     # the per-pin labels identify it; "1" marks pin 1
    j1.Value().SetVisible(False)

    # +5V lanes: post-fuse comes DOWN lane 11.43 into pin 9; pre-fuse goes UP lane 13.97.
    lane_in, lane_out = 5 * PITCH + HALF, 5 * PITCH - HALF          # 13.97, 11.43
    fuse_x = lane_out + 2.1375                                       # pad1 on lane_out
    p.place("F1", fuse_x, STRIP_Y, 0, {"1": "+5V", "2": "+5V_CART"})
    # vertical 0805s, rotation 270 puts pad 1 on top (toward the rail)
    # x positions are midway between finger columns so the far-row labels clear the parts
    for ref, x, pads in (("C1", 19.05, {"1": "+5V", "2": "GND"}),
                         ("C2", 21.59, {"1": "+5V", "2": "GND"}),
                         ("R1", 26.67, {"1": "+5V", "2": "LED_A"})):
        p.place(ref, x, STRIP_Y - 0.5, 270, pads)
    p.place("D1", 26.67, STRIP_Y + 3.0, 90, {"1": "GND", "2": "LED_A"})   # pad2 (A) on top
    for ref in ("F1", "C1", "C2", "R1", "D1"):
        fp = p.fps[ref]
        fp.Value().SetVisible(False)
        r = fp.Reference()
        r.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        r.SetTextThickness(pcbnew.FromMM(0.15))
        r.SetTextAngleDegrees(0)
        cx = pcbnew.ToMM(fp.GetPosition().x) - OX
        cy = pcbnew.ToMM(fp.GetPosition().y) - OY
        if ref in ("R1", "D1"):            # stacked column: refs go beside, not below
            r.SetPosition(V(cx + 1.9, cy))
            r.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
        else:
            r.SetPosition(V(cx, STRIP_Y + (4.7 if ref != "F1" else 2.4)))

    # --- tracks -----------------------------------------------------------
    for n in range(3, 41):
        net = PINS[n][0]
        fx, fy = p.pad_xy("P1", str(n))
        hx, hy = p.pad_xy("J1", str(n))
        assert abs(fx - hx) < 1e-6, f"pin {n}: finger x {fx} != header x {hx}"
        if n == 9:
            continue
        if n % 2:      # odd: F.Cu straight
            p.track(net, F, [(fx, fy), (hx, hy)])
        else:          # even: B.Cu, jog through the lane right of this column
            lane = fx + HALF
            p.track(net, B, [(fx, fy), (fx, Y_NEAR + PITCH), (lane, Y_NEAR + HALF),
                             (lane, Y_NEAR - HALF), (hx, hy)])
    # +5V_CART: finger 9 -> lane_in -> F1 pad 2
    fx, fy = p.pad_xy("P1", "9")
    f2 = p.pad_xy("F1", "2")
    p.track("+5V_CART", F, [(fx, fy), (fx, Y_NEAR + PITCH), (lane_in, Y_NEAR + HALF),
                            (lane_in, STRIP_Y + 3.0), f2], width=0.4)
    # +5V: F1 pad 1 -> down lane_out -> header pin 9; and up to the rail
    f1 = p.pad_xy("F1", "1")
    hx, hy = p.pad_xy("J1", "9")
    p.track("+5V", F, [f1, (lane_out, Y_NEAR - HALF), (hx, hy)], width=0.4)
    p.track("+5V", F, [f1, (lane_out, RAIL_Y), (26.67, RAIL_Y)], width=0.4)
    for ref in ("C1", "C2", "R1"):
        x, y = p.pad_xy(ref, "1")
        p.track("+5V", F, [(x, RAIL_Y), (x, y)], width=0.4)
    r2 = p.pad_xy("R1", "2")
    d2 = p.pad_xy("D1", "2")
    p.track("LED_A", F, [r2, d2])

    # --- outline: rounded rectangle, r = 1.27 on all corners ---------------
    r = 1.27
    k = r * (1 - 2 ** -0.5)
    p.edge_line((X0 + r, 0), (X1 - r, 0))
    p.edge_arc((X1 - r, 0), (X1 - k, -k), (X1, -r))
    p.edge_line((X1, -r), (X1, TOP + r))
    p.edge_arc((X1, TOP + r), (X1 - k, TOP + k), (X1 - r, TOP))
    p.edge_line((X1 - r, TOP), (X0 + r, TOP))
    p.edge_arc((X0 + r, TOP), (X0 + k, TOP + k), (X0, TOP + r))
    p.edge_line((X0, TOP + r), (X0, -r))
    p.edge_arc((X0, -r), (X0 + k, -k), (X0 + r, 0))

    # --- silkscreen -------------------------------------------------------
    L = pcbnew.GR_TEXT_H_ALIGN_LEFT
    for n in range(1, 41):
        label = PINS[n][1]
        x = p.pad_xy("J1", str(n))[0]
        if n % 2:   # near row: label below, reading upward toward the pin
            p.text(label, x, Y_NEAR + 9.5, pcbnew.F_SilkS, rot=90, halign=L)
        else:       # far row: label above the shroud, reading upward away from the pin
            p.text(label, x, y_far - 5.5, pcbnew.F_SilkS, rot=90, halign=L)
    p.text("PIN 1 = RED STRIPE", X0 + 1.0, RAIL_Y - 0.6, pcbnew.F_SilkS, size=0.8, halign=L)
    p.text("PiCoCo CART BREAKOUT v1.0", 26.67, -22.0, pcbnew.F_SilkS, size=1.5, thick=0.25)
    p.text("- - - approx. case surface (CoCo 3) - - -   grip here ^", 26.67, CASE_Y - 0.8,
           pcbnew.F_SilkS, size=0.8)
    p.text("header pin n = cart pin n   cart 1,2 (+/-12V) open, header 1,2 = GND", 26.67, -19.0,
           pcbnew.F_SilkS, size=0.8)
    p.text("PiCoCo CART BREAKOUT v1.0", 26.67, -30.0, pcbnew.B_SilkS, size=2.0, thick=0.3)
    p.text("Tandy Color Computer cartridge edge -> 2x20 header", 26.67, -26.5,
           pcbnew.B_SilkS, size=1.0, thick=0.15)
    p.text("F1 0.5A polyfuse on +5V   power off before inserting", 26.67, -23.0,
           pcbnew.B_SilkS, size=1.0, thick=0.15)
    p.text("Nathan Byrd 2026  CERN-OHL-S-2.0", 26.67, -19.5, pcbnew.B_SilkS, size=1.0, thick=0.15)

    # --- GND pours (kept off the finger tab) ------------------------------
    p.zone(F, "GND", ((X0, FINGER_TOP - 1.0), (X1, TOP)))
    p.zone(B, "GND", ((X0, FINGER_TOP - 1.0), (X1, TOP)))
    p.b.BuildConnectivity()  # island removal needs current connectivity
    pcbnew.ZONE_FILLER(p.b).Fill(p.b.Zones())
    return p.b



# ---------------------------------------------------------------------------
# Cobbler: keyed 2x20 IDC box header on top, two 1x20 pin rows underneath
# straddling the breadboard channel (0.3" apart). Every ribbon conductor
# goes to exactly one breadboard pin. Nothing is bussed.
# ---------------------------------------------------------------------------

COBBLER = "PiCoCo-Cobbler"
COB_FOOTPRINTS = {
    "J1": ("Connector_IDC", "IDC-Header_2x20_P2.54mm_Vertical"),
    "J2": ("Connector_PinHeader_2.54mm", "PinHeader_1x20_P2.54mm_Vertical"),
    "J3": ("Connector_PinHeader_2.54mm", "PinHeader_1x20_P2.54mm_Vertical"),
}
COB_VALUES = {"J1": "RIBBON", "J2": "ODD", "J3": "EVEN"}
COB_SYMBOLS = [
    ("Connector_Generic:Conn_02x20_Odd_Even",
     g.KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_02x20_Odd_Even"),
    ("Connector_Generic:Conn_01x20",
     g.KICAD_STOCK / "Connector_Generic.kicad_sym", "Conn_01x20"),
]
# all 40 conductors pass through 1:1, including the two the breakout leaves open.
# Nothing is joined here, so the two GND conductors and +5V are plain per-conductor
# nets (no power symbols): the breakout is where GND33 and GND34 meet.
COB_NETS = {n: (PINS[n][0] or f"NC{n}") for n in range(1, 41)}
COB_NETS.update({9: "5V", 33: "GND33", 34: "GND34"})
COB_LABELS = {n: PINS[n][1] for n in range(1, 41)}
COB_TOP, COB_BOT = -13.0, 10.5         # board edges (y), header odd row at y = 0
COB_X0, COB_X1 = -4.5, 57.8            # board edges (x), centred on the 2x20 (pin 1 column x = 2.54)


def build_cobbler_schematic() -> str:
    lib_block, pin_map = g.load_symbols(COB_SYMBOLS)
    s = g.Sheet(pin_map)
    s.place("Connector_Generic:Conn_02x20_Odd_Even", "J1", COB_VALUES["J1"], 120.0, 120.0,
            pin_nets={str(n): COB_NETS[n] for n in range(1, 41)},
            footprint=":".join(COB_FOOTPRINTS["J1"]))
    s.place("Connector_Generic:Conn_01x20", "J2", COB_VALUES["J2"], 60.0, 120.0, rot=180,
            pin_nets={str(k): COB_NETS[2 * k - 1] for k in range(1, 21)},
            footprint=":".join(COB_FOOTPRINTS["J2"]))
    s.place("Connector_Generic:Conn_01x20", "J3", COB_VALUES["J3"], 180.0, 120.0,
            pin_nets={str(k): COB_NETS[2 * k] for k in range(1, 21)},
            footprint=":".join(COB_FOOTPRINTS["J3"]))
    title = (
        '  (title_block\n'
        '    (title "PiCoCo Cobbler")\n'
        '    (date "2026-09-05")\n'
        '    (rev "1.0")\n'
        '    (company "Nathan Byrd")\n'
        '    (comment 1 "Keyed 2x20 IDC -> breadboard, strictly 1:1. J2 = odd ribbon pins, J3 = even.")\n'
        '    (comment 2 "Exists because Pi cobblers bus their GND/5V pins, which would short CoCo bus lines")\n'
        '  )'
    )
    return g.assemble(lib_block, s, "A4", title)


def _align_row(p: "Pcb", ref: str, x1: float, y: float) -> None:
    """Rotate a (possibly flipped) 1xN header so pin k sits at x1 + (k-1)*pitch, all at y."""
    for rot in (0, 180, 90, 270):
        p.fps[ref].SetOrientationDegrees(rot)
        p.fps[ref].SetPosition(V(0, 0))
        a, b = p.pad_xy(ref, "1"), p.pad_xy(ref, "2")
        if abs(b[0] - a[0] - PITCH) < 1e-6 and abs(b[1] - a[1]) < 1e-6:
            p.fps[ref].SetPosition(V(x1 - a[0], y - a[1]))
            return
    raise RuntimeError(f"{ref}: could not align header row")


def build_cobbler_pcb() -> pcbnew.BOARD:
    p = Pcb()
    F = pcbnew.F_Cu
    p.place("J1", PITCH, 0, 90, {str(n): COB_NETS[n] for n in range(1, 41)},
            table=COB_FOOTPRINTS, values=COB_VALUES)
    a, b, c = (p.pad_xy("J1", n) for n in ("1", "2", "3"))
    assert abs(c[0] - a[0] - PITCH) < 1e-6 and abs(b[1] - a[1] + PITCH) < 1e-6, (a, b, c)
    y_even = b[1]
    # The shroud outline would cross the pin-row pads underneath; the shroud itself is the key.
    for item in list(p.fps["J1"].GraphicalItems()):
        if item.GetLayer() == pcbnew.F_SilkS:
            p.fps["J1"].Remove(item)
    for ref, y, off in (("J2", PITCH, 1), ("J3", y_even - PITCH, 2)):
        p.place(ref, 0, 0, 0, {str(k): COB_NETS[2 * k - 2 + off] for k in range(1, 21)},
                table=COB_FOOTPRINTS, values=COB_VALUES, back=True)
        _align_row(p, ref, PITCH, y)
    assert abs(p.pad_xy("J3", "1")[1] - p.pad_xy("J2", "1")[1] + 7.62) < 1e-6, "rows must be 0.3in apart"
    for fp in p.fps.values():
        fp.Value().SetVisible(False)
        fp.Reference().SetVisible(False)
    for n in range(1, 41):
        hx, hy = p.pad_xy("J1", str(n))
        ref, k = ("J2", (n + 1) // 2) if n % 2 else ("J3", n // 2)
        bx, by = p.pad_xy(ref, str(k))
        assert abs(bx - hx) < 1e-6, (n, hx, bx)
        p.track(COB_NETS[n], F, [(hx, hy), (bx, by)])

    r = 1.0
    k = r * (1 - 2 ** -0.5)
    X0, X1, T, Bm = COB_X0, COB_X1, COB_TOP, COB_BOT
    p.edge_line((X0 + r, Bm), (X1 - r, Bm))
    p.edge_arc((X1 - r, Bm), (X1 - k, Bm - k), (X1, Bm - r))
    p.edge_line((X1, Bm - r), (X1, T + r))
    p.edge_arc((X1, T + r), (X1 - k, T + k), (X1 - r, T))
    p.edge_line((X1 - r, T), (X0 + r, T))
    p.edge_arc((X0 + r, T), (X0 + k, T + k), (X0, T + r))
    p.edge_line((X0, T + r), (X0, Bm - r))
    p.edge_arc((X0, Bm - r), (X0 + k, Bm - k), (X0 + r, Bm))

    L = pcbnew.GR_TEXT_H_ALIGN_LEFT
    for n in range(1, 41):
        x = p.pad_xy("J1", str(n))[0]
        if n % 2:   # odd row is below the shroud: label reads upward toward it
            p.text(COB_LABELS[n], x, Bm - 0.6, pcbnew.F_SilkS, rot=90, halign=L)
        else:       # even row above the shroud
            p.text(COB_LABELS[n], x, y_even - 5.5, pcbnew.F_SilkS, rot=90, halign=L)
    p.text("1", X0 + 1.2, PITCH + 0.2, pcbnew.F_SilkS, size=1.0, thick=0.2)
    p.text("PiCoCo COBBLER v1.0  2x20 IDC -> breadboard, 1:1, nothing bussed",
           (X0 + X1) / 2, Bm - 1.6, pcbnew.B_SilkS, size=1.0, thick=0.15)
    p.text("odd ribbon pins -> this row", (X0 + X1) / 2, PITCH + 2.0, pcbnew.B_SilkS, size=0.8)
    p.text("even ribbon pins -> this row", (X0 + X1) / 2, y_even - PITCH - 2.2, pcbnew.B_SilkS, size=0.8)
    return p.b

# ---------------------------------------------------------------------------
# Project file + lib tables
# ---------------------------------------------------------------------------

def write_project(name: str) -> None:
    pro = json.loads((ROOT / "PiCoCo" / "PiCoCo.kicad_pro").read_text())
    pro["meta"] = {"filename": f"{name}.kicad_pro", "version": 3}
    pro["sheets"] = []
    rules = pro["board"]["design_settings"]["rules"]
    rules.update({
        "min_clearance": 0.15, "min_track_width": 0.2, "min_via_diameter": 0.5,
        "min_through_hole_diameter": 0.3, "min_copper_edge_clearance": 0.3,
        "min_hole_clearance": 0.25, "min_hole_to_hole": 0.25,
        "min_text_height": 0.8, "min_text_thickness": 0.12, "min_silk_clearance": 0.0,
    })
    pro["net_settings"]["classes"] = [{
        "bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
        "diff_pair_width": 0.2, "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1,
        "name": "Default", "pcb_color": "rgba(0, 0, 0, 0.000)", "priority": 2147483647,
        "schematic_color": "rgba(0, 0, 0, 0.000)", "track_width": 0.3, "tuning_profile": "",
        "via_diameter": 0.8, "via_drill": 0.4, "wire_width": 6,
    }]
    pro["net_settings"]["netclass_patterns"] = []
    (OUT / f"{name}.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
    for t in ("fp-lib-table", "sym-lib-table"):
        (OUT / t).write_text((ROOT / "PiCoCo" / t).read_text())


def main() -> None:
    OUT.mkdir(exist_ok=True)
    write_fingers_footprint()
    write_fingers_symbol()
    for name, sch_fn, pcb_fn in ((NAME, build_schematic, build_pcb),
                                 (COBBLER, build_cobbler_schematic, build_cobbler_pcb)):
        g.PROJECT_NAME = name
        (OUT / f"{name}.kicad_sch").write_text(sch_fn())
        pcbnew.SaveBoard(str(OUT / f"{name}.kicad_pcb"), pcb_fn())
        write_project(name)
        print(f"wrote {OUT}/{name}.kicad_{{sch,pcb,pro}}")


if __name__ == "__main__":
    main()
