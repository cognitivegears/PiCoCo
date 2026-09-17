#!/usr/bin/env python3
"""Asserts the v2.3 library items exist and are geometrically right. Stdlib only."""
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
PRETTY = ROOT / "libraries" / "PiCoCo.pretty"
SYMS = ROOT / "libraries" / "PiCoCo.kicad_sym"

GRID = {  # pad number -> (x, y) in the footprint frame, RP2350B_IDEAS §13.4
    "GP26": (-5.08, 22.40), "GP29": (-2.54, 22.40), "GP32": (0.0, 22.40), "GP35": (2.54, 22.40), "GP45": (5.08, 22.40),
    "GP25": (-5.08, 19.86), "GP28": (-2.54, 19.86), "GP31": (0.0, 19.86), "GP34": (2.54, 19.86), "GP44": (5.08, 19.86),
    "GP24": (-5.08, 17.32), "GP27": (-2.54, 17.32), "GP30": (0.0, 17.32), "GP33": (2.54, 17.32), "GP43": (5.08, 17.32),
}
# GP29/GP32/GP35 sit closest to the (now-removed) Pico 2 debug pads and are
# shrunk to 1.4x1.4 so a bare Pico 2's own debug-pad copper still clears.
SMALL_GRID_PADS = ("GP29", "GP32", "GP35")

PAD_LINE = re.compile(
    r'\(pad "([^"]*)" (\w+) \w+ \(at ([-\d.]+) ([-\d.]+)[^)]*\) \(size ([\d.]+) ([\d.]+)\)[^\n]*?\(layers ([^)]*)\)'
)
DRILL_NUM = re.compile(r'\(drill ([\d.]+)\)')


def pads(text):
    """pad number -> list of records (one per line, e.g. thru_hole + smd)."""
    out = {}
    for line in text.splitlines():
        m = PAD_LINE.search(line)
        if not m:
            continue
        num, kind, x, y, w, h, layers = m.groups()
        dm = DRILL_NUM.search(line)
        out.setdefault(num, []).append({
            "kind": kind, "x": float(x), "y": float(y),
            "w": float(w), "h": float(h),
            "drill": float(dm.group(1)) if dm else None,
            "layers": layers,
        })
    return out


def _boxes(rec):
    """Axis-aligned boxes (x0, x1, y0, y1) a pad record occupies: its copper,
    plus its drilled hole (if any) since a hole needs clearance from other
    pads' copper too, not just its own."""
    x, y, w, h = rec["x"], rec["y"], rec["w"], rec["h"]
    boxes = [(x - w / 2, x + w / 2, y - h / 2, y + h / 2)]
    if rec["kind"] == "thru_hole" and rec["drill"]:
        d = rec["drill"]
        boxes.append((x - d / 2, x + d / 2, y - d / 2, y + d / 2))
    return boxes


def _axis_gap(a0, a1, b0, b1):
    """Positive = separated by that much; negative = overlap depth."""
    if a1 <= b0:
        return b0 - a1
    if b1 <= a0:
        return a0 - b1
    return -(min(a1, b1) - max(a0, b0))


def _clear(box_a, box_b, min_gap=0.15):
    gx = _axis_gap(box_a[0], box_a[1], box_b[0], box_b[1])
    gy = _axis_gap(box_a[2], box_a[3], box_b[2], box_b[3])
    return gx >= min_gap or gy >= min_gap


def check_pad_clearances(p, min_gap=0.15):
    """Every pair of *different-numbered* pads must clear by >=min_gap in X or Y."""
    nums = [n for n in p if n]  # skip unnumbered (NPTH mounting) pads
    for i, n1 in enumerate(nums):
        for n2 in nums[i + 1:]:
            for r1 in p[n1]:
                for r2 in p[n2]:
                    for b1 in _boxes(r1):
                        for b2 in _boxes(r2):
                            assert _clear(b1, b2, min_gap), f"pad {n1} too close to pad {n2}"


def main():
    fp = (PRETTY / "Pico-Carrier.kicad_mod").read_text()
    src = (PRETTY / "RPi_Pico_SMD_TH.kicad_mod").read_text()
    assert fp.startswith('(footprint "Pico-Carrier"'), "footprint name"
    p = pads(fp)
    for num in ("41", "42", "43"):
        assert num not in p, f"debug pad {num} collides with the Plus-W grid and must be removed"
    for num, (x, y) in GRID.items():
        assert num in p, f"missing pad {num}"
        rec = p[num][0]
        assert rec["kind"] == "smd" and abs(rec["x"] - x) < 0.005 and abs(rec["y"] - y) < 0.005, f"{num} at {rec['x']},{rec['y']}"
        expected = 1.4 if num in SMALL_GRID_PADS else 1.8
        assert abs(rec["w"] - expected) < 0.005 and abs(rec["h"] - expected) < 0.005, f"{num} size"
        assert "F.Cu" in rec["layers"], f"{num} layers {rec['layers']}"
    for num in ("1", "20", "21", "40"):
        assert num in p, f"missing header pad {num}"
    assert "F.Paste" not in fp, "footprint has paste"
    assert "antenna" in fp.lower(), "antenna keepout text missing"
    assert re.search(r'\(fp_rect \(start -10\.5 -25\.5\) \(end 10\.5 30\.42\)', fp), "courtyard extent"
    assert fp.count('(layer "F.CrtYd")') == 1, "expected exactly one courtyard outline"
    # Regression guard: a cross-item courtyard-strip regex once ate every
    # silkscreen position marker and copper-keepout polygon between the
    # first F.SilkS fp_line and the first F.CrtYd one. Carrier keeps every
    # inherited F.SilkS/fp_poly item and adds exactly 2 silkscreen labels
    # (USB, ANT) of its own.
    assert fp.count('(layer "F.SilkS")') == src.count('(layer "F.SilkS")') + 2, (
        "F.SilkS item count changed unexpectedly (expected the inherited count plus "
        "the 2 USB/ANT labels this footprint adds)"
    )
    assert fp.count("(fp_poly") == src.count("(fp_poly"), "fp_poly (copper keepout) count changed"
    check_pad_clearances(p)

    cart = (PRETTY / "COCO-CART-2.1X1.75.kicad_mod").read_text()
    assert "-5.207) (size 1.27 9.525)" not in cart, "fingers still end 0.44 mm from the edge"
    assert cart.count("-5.635) (size 1.27 8.67)") == 39, "39 trimmed fingers expected (pin 9 is short already)"

    sym = SYMS.read_text()
    assert '(symbol "Pico-Carrier"' in sym and '(symbol "74LVC00"' in sym
    car = sym[sym.index('(symbol "Pico-Carrier"'):]
    car = car[:car.index('\n  (symbol "', 10)] if '\n  (symbol "' in car[10:] else car
    for num in GRID:
        assert f'(number "{num}"' in car, f"symbol pin {num}"
    for num in ("41", "42", "43"):
        assert f'(number "{num}"' not in car, f"symbol pin {num} should be removed"
    assert '(name "GP26/GP40"' in car and '(name "GP27/GP41"' in car and '(name "GP28/GP42"' in car
    nand = sym[sym.index('(symbol "74LVC00"'):]
    nand = nand[:nand.index('\n  (symbol "', 10)] if '\n  (symbol "' in nand[10:] else nand
    for n in range(1, 15):
        assert f'(number "{n}"' in nand, f"74LVC00 pin {n}"
    assert nand.count("(pin ") >= 14
    print("library ok")

if __name__ == "__main__":
    main()
