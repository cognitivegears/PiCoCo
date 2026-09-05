#!/usr/bin/env python3
"""
Rough auto-placement for the PiCoCo PCB.

Moves each footprint's `(at X Y [rot])` to a planned grid position
on the board. P1 (cart edge connector) is left untouched; all other
footprints are repositioned.

Board geometry (from the COCO-CART footprint placed at P1 origin
121.92, 109.347):
    x = 99.59 .. 197.59  (98 mm wide)
    y = 44.187 .. 99.187 (55 mm tall, edge fingers extend below)

The layout clusters components by signal path:
    - Bottom row (y 90-97): level shifters U10-U13, U15, LDO U14
    - Middle (y 65-80): Pico 2 (U1) horizontal
    - Top (y 45-60): debug headers, pull-ups
"""

from __future__ import annotations

import re
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
PCB = PROJECT_ROOT / "PiCoCo" / "PiCoCo.kicad_pcb"

# Ref -> (x, y, rotation)
# P1 omitted: we preserve its current placement.
#
# Board extents (absolute, derived from P1 at 121.92, 109.347):
#   x: 99.59 .. 197.59  (98 mm wide)
#   y: 44.187 .. 99.187 (55 mm tall)
# 3 mm margin inside: x 102.59..194.59, y 47.187..96.187.
#
# The two 2x20 headers (J_CART1, J_LVC1) are 50.8 x 5.08 mm each and
# just don't fit alongside a horizontal Pico on a 98x55 board. We
# park them OFF the board (y around 25) so you can decide: delete
# them, shrink to 1x20, or expand the board. Same for the SWD header
# which we'll place just above the board top edge.

PLACEMENT: dict[str, tuple[float, float, float]] = {
    # v2.2: J_CART and J_LVC debug headers removed — they dominated
    # the 98x55 mm board area. Use TP1..TP6 pads + J_SWD + USB CDC for
    # bring-up debug instead.

    # -------- Pi Pico 2: vertical, right edge of the board --------
    # Pico footprint is 17.8 wide x 48.3 tall. Center at (188, 72)
    # puts the body at x 179.1..196.9, y 47.85..96.15 (fits inside
    # 44.187..99.187 with ~3 mm margin top and bottom).
    "U1":  (188.0, 72.0,  0),

    # -------- Middle row (y~63): pull-ups, decoupling --------
    "R1":  (106.0, 63.0,  0),       # /HALT pull-up
    "R2":  (113.0, 63.0,  0),       # /NMI pull-up
    "R3":  (120.0, 63.0,  0),       # /RESET pull-up
    "R4":  (127.0, 63.0,  0),       # 3V3_EN pull-up
    "C10": (148.0, 63.0,  0),       # Pico local decoupling

    # -------- SWD header: between Pico & middle row --------
    "J_SWD1": (138.0, 73.0, 90),    # 1x4 vertical, compact

    # -------- Decoupling row (y=84), above each IC ----------
    "C2":  (105.0, 84.0,  90),      # LDO input cap
    "C3":  (113.0, 84.0,  90),      # LDO output cap
    "C6":  (124.0, 84.0,  90),      # U11
    "C7":  (141.0, 84.0,  90),      # U12
    "C8":  (155.0, 84.0,  90),      # U13
    "C4":  (164.0, 84.0,  90),      # U10 Vcca (3V3)
    "C5":  (174.0, 84.0,  90),      # U10 Vccb (+5V)
    "C9":  (170.0, 75.0,  90),      # U15 (moved above U15 to free overlap)

    # -------- Shifter row (y=92) + LDO: closest to P1 --------
    # 16-mm pitch between SOIC-20s; U10 gets extra space for its
    # SOIC-24 body (15.4 mm long). Clearance to Pico (x=179.1) is
    # enforced by keeping U10 center ≤ 170.
    "U14": (108.0, 92.0,  0),       # AMS1117-3.3 LDO
    "U11": (124.0, 92.0,  0),       # A0-A7 buffer
    "U12": (141.0, 92.0,  0),       # A8-A13 + /R/W + /CTS + /SCS
    "U13": (154.0, 92.0,  0),       # ctrl2 (E, Q, etc.)
    "U10": (170.0, 92.0,  0),       # Data bus bidi (wider SOIC-24)
    "U15": (170.0, 78.0,  0),       # AND gate, tiny, between U10 and Pico

    # -------- C1: +5V bulk near cart edge, west of U11 --------
    # Previous (120, 97) overlapped U11 top pads (y=95.0..96.5).
    # (115, 97.5) keeps 0805 pad edges ≥1 mm from U11 and ≥0.5 mm
    # from the bottom board edge.
    "C1":  (115.0, 97.5,  0),

    # -------- C11: +5V bulk local to U10 Vccb --------
    # Previous (177, 88) overlapped U10 pin 12. (176.5, 97.5) straddles
    # the column above U10's Vccb pins (23/24) with clean board-edge
    # margin and stays clear of the Pico footprint (x≥179.1).
    "C11": (176.5, 97.5,  0),

    # -------- D2: Schottky from +5V to VSYS_PICO, north of LDO --------
    # Previous (100, 88) extended past the board's left edge (x=99.59).
    # (115, 96) sits in open space between U14 and U12 with pads
    # clear of both.
    "D2":  (115.0, 96.0,  0),

    # -------- /HALT firmware-drive stack: Q2 + R7 + R8 --------
    # Q2 sinks /HALT; keep it near the cart-side /HALT trace. R7 pulls
    # gate high during Pico boot, R8 isolates GP27 from gate transients.
    "Q2":  (108.0, 85.0,  0),
    "R7":  (134.0, 63.0,  0),       # Q2 gate pull-up to +3V3
    "R8":  (141.0, 63.0,  0),       # Q2 gate series from GP27

    # -------- /CART pull-up with user jumper --------
    "R6":  (102.0, 97.0,  0),       # 4.7k to +5V
    "JP1": (107.0, 97.0,  0),       # Install shunt = /CART pull-up active

    # -------- Pico RUN from CoCo /RESET --------
    # Previous (181, 85/88) sat INSIDE the Pico body (x 179.1..196.9,
    # y 47.85..96.15). Moved south of the Pico into the free strip
    # between Pico's bottom edge and the board's bottom edge.
    "R9":  (185.0, 97.5,  0),       # RUN series
    "R10": (190.0, 97.5,  0),       # RUN pull-up

    # -------- Series termination on fan-out nets --------
    # R11 nudged north to clear U15 top pads (y≈79.25).
    # R12 moved below U12 (U12 top pads at y≈96.25); tight against
    # board edge but fits.
    "R11": (175.0, 81.0,  0),       # OE_BUS near U15 output
    "R12": (147.0, 97.5,  0),       # RW_BUF south of U12

    # -------- SWD series protection --------
    "R13": (132.0, 73.0,  0),       # SWCLK
    "R14": (144.0, 73.0,  0),       # SWDIO

    # -------- Test points (1x1 mm SMD pads) --------
    # Moved off the IC pad rows; adjacent-to-IC spots were overlapping
    # pin pads. Current positions are in clear space below/above the
    # relevant IC (tracks will route up to them during hand-route).
    "TP1": (172.0, 74.0,  0),       # OE_BUS
    "TP2": (150.0, 85.5,  0),       # RW_BUF
    "TP3": (138.0, 86.0,  0),       # CTS_BUF
    "TP4": (153.0, 86.0,  0),       # SCS_BUF
    "TP5": (158.0, 86.0,  0),       # E_B
    "TP6": (112.0, 66.0,  0),       # +3V3
}


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


def main() -> None:
    text = PCB.read_text()
    # Iterate footprint blocks, rewriting each that has a matching ref
    changes = 0
    out_parts: list[str] = []
    last_end = 0
    i = 0
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
        x, y, rot = PLACEMENT[ref]
        # Find FIRST (at X Y [rot]) inside this footprint (the top-level
        # placement). It's the first occurrence AFTER the footprint name,
        # and before any (property ...).
        # More specifically: we want to rewrite the top-level (at X Y [rot]).
        # Use a regex that matches a standalone (at ...) on its own line.
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
    PCB.write_text("".join(out_parts))
    print(f"moved {changes} footprints")


if __name__ == "__main__":
    main()
