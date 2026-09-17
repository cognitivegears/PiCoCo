#!/usr/bin/env python3
"""
Auto-placement for the PiCoCo v2.3 PCB.

Moves each footprint's `(at X Y [rot])` to a planned grid position on the
board, then appends board-level extras (antenna/HDMI keepout zones and
silkscreen texts) idempotently. P1 (cart edge connector) is left untouched;
all other footprints named in PLACEMENT are repositioned.

Board geometry (from the COCO-CART footprint placed at P1 origin
121.92, 109.347):
    x = 99.59 .. 197.59  (98 mm wide)
    y = 44.187 .. 99.187 (55 mm tall, edge fingers extend below)

v2.3 layout:
    - Pico-Carrier module (U1) horizontal along the top-left edge, USB end
      flush with the left board edge, antenna end pointing right into a
      keepout zone.
    - Power block (LDO, Schottky, bulk caps) near the +5V finger.
    - /HALT, /NMI, /CART gate stages near fingers 3/4/8.
    - Buffer row (U10-U13, U15) nearest the cart fingers, in finger order.
    - Sound stage in a line to the SND finger.
    - Test points, C10 and JP3 between the module and the reserved HDMI
      corner (top-right), which is left empty for a future HDMI connector.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
PCB = PROJECT_ROOT / "PiCoCo" / "PiCoCo.kicad_pcb"

# Ref -> (x, y, rotation), board mm, KiCad Y down.
#
# Board: x 99.59..197.59, y 44.187..99.187, fingers at the bottom.
# Finger x positions: cart pad n at x = 121.92 + 2.54*ceil(n/2) (pins 3/4 at
# 127.0, 9 at 134.6, D0..D7 134.6..142.2, A0..A7 144.8..152.4, A8..A12
# 155.0..160.0, /CTS 160.0, /SCS 167.6, A13/A14 168.9, A15//SLENB 171.5,
# SND 167.6).
#
# Reserved HDMI corner: x 172.6..197.6, y 44.2..59.2 (nothing placed there).
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
    # Between module and the reserved HDMI corner: JP3 by module pin 34, test points, C10
    "JP3": (152.0, 62.0, 0), "C10": (152.0, 66.0, 90),
    "TP1": (166.0, 50.0, 0), "TP2": (166.0, 54.0, 0), "TP3": (166.0, 58.0, 0),
    "TP4": (166.0, 62.0, 0), "TP5": (166.0, 66.0, 0), "TP6": (170.0, 50.0, 0),
    # Fiducials 3 mm in from two diagonal corners
    "FID1": (102.6, 47.2, 0), "FID2": (194.6, 96.2, 0),
}


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
    """Append EXTRAS zones and TEXTS silkscreen, idempotently, before the file's final `)`."""
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


def _find_footprint_block(text: str, start: int) -> tuple[int, int]:
    """Return (start, end) of the balanced (footprint ...) block."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return (start, i + 1)
        i += 1
    raise RuntimeError("unbalanced footprint block")


def main() -> int:
    text = PCB.read_text()
    # Iterate footprint blocks, rewriting each that has a matching ref
    changes = 0
    seen: set[str] = set()
    out_parts: list[str] = []
    last_end = 0
    for m in re.finditer(r'\(footprint\s+"[^"]+"', text):
        fp_start, fp_end = _find_footprint_block(text, m.start())
        block = text[fp_start:fp_end]
        # ref
        ref_m = re.search(r'\(property\s+"Reference"\s+"([^"]+)"', block)
        if not ref_m:
            out_parts.append(text[last_end:fp_end])
            last_end = fp_end
            continue
        ref = ref_m.group(1)
        if ref not in PLACEMENT:
            out_parts.append(text[last_end:fp_end])
            last_end = fp_end
            continue
        seen.add(ref)
        x, y, rot = PLACEMENT[ref]
        # Find FIRST (at X Y [rot]) inside this footprint (the top-level
        # placement). It's the first occurrence AFTER the footprint name,
        # and before any (property ...).
        new_at = f'(at {x:.3f} {y:.3f}{f" {int(rot)}" if rot else ""})'
        # Look for the first `(at N N [N])` at minimum indent after the
        # footprint header
        at_re = re.compile(
            r'\n(\t+)\(at\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?\)',
        )
        m2 = at_re.search(block)
        if not m2:
            out_parts.append(text[last_end:fp_end])
            last_end = fp_end
            continue
        block2 = block[:m2.start()] + f'\n{m2.group(1)}' + new_at + block[m2.end():]
        out_parts.append(text[last_end:fp_start])
        out_parts.append(block2)
        last_end = fp_end
        changes += 1
    out_parts.append(text[last_end:])
    out_text = "".join(out_parts)
    out_text = add_board_extras(out_text)
    PCB.write_text(out_text)
    print(f"moved {changes} footprints")

    missing = sorted(set(PLACEMENT) - seen)
    for ref in missing:
        print(f"missing: {ref}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
