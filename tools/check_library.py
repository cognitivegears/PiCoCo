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
