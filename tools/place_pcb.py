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
    # Verified: rot=90 puts pad 1 (USB) at x=101.0 (USB/left edge) and pad 20 at
    # x=149.2 (antenna/right end), matching spec. (An earlier hand-derived check with
    # a mis-signed rotation matrix wrongly suggested 270; 270 also collides pad 38-40
    # with P1's MTG1 mounting hole. 90 is correct and collision-free.)
    "U1":   (125.1, 56.2, 90),
    # Power block near the +5V finger (x 134.6): LDO, Schottky, bulk caps
    # U14/C1 nudged +/-3mm in x from the brief to give D2's 7mm-wide courtyard
    # clearance on both sides (measured courtyard overlap at the brief's coords).
    "U14":  (105.0, 92.0, 0), "D2": (115.0, 96.0, 0), "C1": (124.0, 97.5, 0), "C2": (105.0, 84.0, 90),
    "C3":   (113.0, 84.0, 90), "C12": (106.0, 74.0, 0), "R4": (118.0, 70.0, 0),
    # /HALT, /NMI, /CART stages near fingers 3/4/8 (x 127..132)
    "R1": (124.0, 86.0, 0), "Q2": (128.0, 85.0, 0), "R7": (124.0, 80.0, 0), "R8": (128.0, 80.0, 0),
    "R2": (132.0, 86.0, 0), "Q3": (132.0, 80.0, 0), "R15": (136.0, 80.0, 0), "R17": (136.0, 76.0, 0),
    "Q4": (128.0, 74.0, 0), "R16": (132.0, 74.0, 0), "R18": (132.0, 70.0, 0),
    "R3": (140.0, 70.0, 0),
    # Buffer row nearest the fingers, in finger order: data, A0-7, A8-13/RW/CTS, controls
    "U10": (140.0, 92.0, 0), "U11": (154.0, 92.0, 0), "U12": (168.0, 92.0, 0), "U13": (182.0, 92.0, 0),
    # Decoupling caps nudged from y=84.0 to 83.0 (brief's y=84 clipped the buffer
    # ICs' courtyard, which starts at y=85.35, by ~0.35mm; measured after placement).
    "C4": (140.0, 83.0, 90), "C6": (154.0, 83.0, 90), "C7": (168.0, 83.0, 90), "C8": (182.0, 83.0, 90),
    "U15": (175.0, 77.0, 0), "C9": (181.0, 77.0, 90),
    "R11": (175.0, 71.0, 0), "R12": (163.0, 84.0, 0), "JP2": (146.0, 84.0, 0),
    "R9": (190.0, 84.0, 0), "R10": (194.0, 84.0, 90),
    # Sound stage: regridded 2 rows x 4 cols (rot 90 on the 0805s) right of U13
    # (courtyard ends x=187.93) and clear of FID2's reserved corner (measured
    # courtyard: FID2 circle spans x 193.35..195.85, y 94.95..97.45). The brief's
    # single-row layout put R19/C13/R20 inside both U13's courtyard and FID2's.
    "R21": (189.5, 87.5, 90), "R22": (191.7, 87.5, 90), "C15": (193.9, 87.5, 90), "TP7": (196.1, 87.5, 0),
    "R19": (189.5, 91.5, 90), "C13": (191.7, 91.5, 90), "R20": (193.9, 91.5, 90), "C14": (196.1, 91.5, 90),
    # Between module and the reserved HDMI corner: JP3 by module pin 34, test points, C10.
    # JP3/C10 nudged +8mm in x from the brief: at x=152 both sat inside U1's
    # courtyard (ends x=155.52) and the antenna keepout (x 150.6..155.6).
    "JP3": (160.0, 62.0, 0), "C10": (160.0, 66.0, 90),
    "TP1": (166.0, 50.0, 0), "TP2": (166.0, 54.0, 0), "TP3": (166.0, 58.0, 0),
    "TP4": (166.0, 62.0, 0), "TP5": (166.0, 66.0, 0), "TP6": (170.0, 50.0, 0),
    # Fiducials 3 mm in from two diagonal corners. FID1 at (102.6, 80.0) per
    # review (measured clear with 1.5mm margin there); earlier fallbacks
    # (102.6, 47.2) sat inside U1's pad clearance/courtyard, (102.6, 70.0)
    # inside C12's courtyard, and (114.0, 76.0) worked but this lengthens
    # the fiducial diagonal, which review preferred.
    "FID1": (102.6, 80.0, 0), "FID2": (194.6, 96.2, 0),
}


def _zone_keepout(name: str, layers: str, pts: list[tuple[float, float]], what: str) -> str:
    poly = " ".join(f"(xy {x:.3f} {y:.3f})" for x, y in pts)
    return (f'  (zone (net 0) (net_name "") (layers {layers}) (name "{name}") (hatch edge 0.5)\n'
            f'    (keepout {what})\n    (polygon (pts {poly}))\n  )\n')


EXTRAS = [
    # Antenna: starts at x=151.3, not the module's nominal edge (150.6) -- pad 20/21's
    # castellated copper legitimately overhangs the nominal edge by ~0.4mm (measured:
    # pad 20 copper right edge lands at x=150.98), so a zone starting exactly at 150.6
    # clips it (DRC items_not_allowed). 151.3 clears it with margin; still 4.3mm wide.
    # No "footprints not_allowed": the module's OWN antenna area (part of U1's
    # courtyard by design) legitimately overlaps this zone; the rule's actual intent
    # is keeping OTHER components' copper out, which tracks/vias/pads/copperpour cover.
    _zone_keepout("antenna_keepout", '"F.Cu" "B.Cu"',
                  [(151.3, 45.7), (155.6, 45.7), (155.6, 66.7), (151.3, 66.7)],
                  "(tracks not_allowed) (vias not_allowed) (pads not_allowed) (copperpour not_allowed)"),
    # Reserved HDMI corner: no footprints, routing allowed.
    _zone_keepout("hdmi_reserved", '"F.Cu"',
                  [(172.6, 44.2), (197.6, 44.2), (197.6, 59.2), (172.6, 59.2)],
                  "(tracks allowed) (vias allowed) (pads not_allowed) (copperpour allowed) (footprints not_allowed)"),
]


def _u1_npth_keepouts() -> list[str]:
    """Track/via keepouts round U1's four NPTH holes (USB-connector legs at the
    module's USB end). The board's 0.25 mm hole-clearance rule can't be expressed
    in a Specctra DSN, so Freerouting kept laying VSYS_PICO across a hole edge
    (DRC hole_clearance, 0.22 mm). A square keepout of hole radius + 0.25 +
    0.05 keeps the router honest; copper pour is still allowed (zone fill obeys
    the hole clearance itself)."""
    ux, uy, rot = PLACEMENT["U1"]
    assert rot == 90, "NPTH keepouts assume U1 rotated 90 (x,y)->(y,-x)"
    holes = [(-2.725, -24.0, 1.8), (-2.425, -20.97, 1.5), (2.425, -20.97, 1.5), (2.725, -24.0, 1.8)]
    out = []
    for i, (lx, ly, d) in enumerate(holes, 1):
        cx, cy = ux + ly, uy - lx
        h = d / 2 + 0.25 + 0.05
        out.append(_zone_keepout(f"u1_npth_{i}", '"F.Cu" "B.Cu"',
                                 [(cx - h, cy - h), (cx + h, cy - h), (cx + h, cy + h), (cx - h, cy + h)],
                                 "(tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour allowed)"))
    return out


EXTRAS += _u1_npth_keepouts()
TEXTS = [  # (text, x, y, layer, size)
    ("HDMI (future)", 185.0, 51.5, "F.SilkS", 1.0),
    # Version/licence + URL moved to the back silkscreen (review F5): on the
    # front they crossed U1's lower pin labels and R3. Mirrored like the JLC
    # text, centred in the empty back-side area below the module.
    ("PiCoCo v2.3  CERN-OHL-S-2.0", 150.0, 75.0, "B.SilkS", 1.0),
    ("github.com/cognitivegears/PiCoCo", 150.0, 78.0, "B.SilkS", 0.8),
    ("JLCJLCJLCJLC", 110.0, 62.0, "B.SilkS", 1.0),
    # Shifted right from x=146 (review F5): the left end touched R15 (at x=136).
    ("JP2 1-2=HW /OE  2-3=FW", 152.0, 80.5, "F.SilkS", 0.8),
    # Moved off the module (review F2): (152, 59) sat inside U1's courtyard/pad
    # grid. Split across two lines: at 0.8mm the full legend measures 20.5mm
    # wide (measured via pcbnew), wider than the 16.6mm corridor between U1's
    # courtyard (ends x=155.52) and the HDMI reserve (starts x=172.6) -- one
    # line always bled into either U1 or R11 (which sits just past x=172.6).
    # Two lines fit that corridor comfortably and the y=68.5..71.5 band is
    # clear (below C10, above R12/U12's row at y>=82).
    ("JP3 1-2=E", 164.0, 69.0, "F.SilkS", 0.8),
    ("2-3=AUDIO (Pico2)", 164.0, 71.0, "F.SilkS", 0.8),
    ("no parts under module", 125.0, 62.0, "F.Fab", 1.0),
]

# Old v0.1 gr_text banner, left behind by the pre-v2.3 board; it now sits
# under the module and must be deleted (structural item removal, not a
# regex, since its content spans multiple lines).
_STALE_TEXT_PREFIX = '(gr_text "PiCoCo\\nUniversal Cartridge'

# Superseded TEXTS entries: a text whose content string changed (not just
# its position/size/layer, which _upsert_item's marker-based replace
# already handles) needs an explicit one-time removal, since a changed
# content string is a different marker and won't be found/replaced.
_RETIRED_TEXTS = [
    "JP3 1-2=E  2-3=AUDIO (Pico2)",  # split into two lines (review F2)
]


def _upsert_item(body: str, item_text: str, marker: str, block_start_token: str | None = None) -> str:
    """Ensure `item_text` is present in `body`, exactly once.

    If `item_text` is already present verbatim, no-op. Otherwise, if a
    stale item matching `marker` exists (same identity -- a zone name, a
    gr_text's content string -- but different content, e.g. a moved
    position or resized text), remove that whole item first so the
    replacement isn't a second, duplicate copy. `block_start_token` is the
    token the item's own balanced block starts with (e.g. "(zone"); pass
    None when `marker` itself IS that start (e.g. a gr_text's own opening
    substring).
    """
    if item_text in body:
        return body
    idx = body.find(marker)
    if idx != -1:
        item_start = body.rfind(block_start_token, 0, idx + 1) if block_start_token else idx
        i_start, i_end = _find_footprint_block(body, item_start)
        line_start = body.rfind("\n", 0, i_start) + 1
        body = body[:line_start] + body[i_end:]
    return body.rstrip("\n") + "\n" + item_text


def add_board_extras(text: str) -> str:
    """Append EXTRAS zones and TEXTS silkscreen, idempotently, before the file's final `)`."""
    end = text.rstrip().rfind(")")
    body, tail = text[:end], text[end:]
    stale = body.find(_STALE_TEXT_PREFIX)
    if stale != -1:
        item_start, item_end = _find_footprint_block(body, stale)
        # eat the leading newline+indent too, else a blank line is left behind
        line_start = body.rfind("\n", 0, item_start) + 1
        body = body[:line_start] + body[item_end:]
    for old_text in _RETIRED_TEXTS:
        idx = body.find(f'(gr_text "{old_text}"')
        if idx != -1:
            item_start, item_end = _find_footprint_block(body, idx)
            line_start = body.rfind("\n", 0, item_start) + 1
            body = body[:line_start] + body[item_end:]
    for z in EXTRAS:
        name = re.search(r'\(name "([^"]+)"', z).group(1)
        body = _upsert_item(body, z, f'(name "{name}")', "(zone")
    for t, x, y, layer, size in TEXTS:
        justify = " (justify mirror)" if layer.startswith("B.") else ""
        item = (f'  (gr_text "{t}" (at {x} {y} 0) (layer "{layer}")\n'
                f'    (effects (font (size {size} {size}) (thickness {size*0.15:.2f})){justify})\n  )\n')
        body = _upsert_item(body, item, f'(gr_text "{t}"')
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


_ANGLE_AT_RE = re.compile(r'\(at\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?(\s+unlocked)?\)')
_ANGLE_ITEM_RE = re.compile(r'\((pad|fp_text|property)\s')

STOCK_FOOTPRINTS = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
LOCAL_PRETTY = PROJECT_ROOT / "libraries" / "PiCoCo.pretty"


def _fmt_angle(a: float) -> str:
    a %= 360
    if abs(a) < 1e-9:
        a = 0.0
    return str(int(round(a))) if abs(a - round(a)) < 1e-6 else f'{a:.6g}'


def _footprint_lib_path(lib_id: str) -> Path | None:
    """Resolve a placed footprint's lib_id ("Lib:Name") to its .kicad_mod file."""
    if ":" not in lib_id:
        return None
    lib, name = lib_id.split(":", 1)
    base = LOCAL_PRETTY if lib == "PiCoCo" else STOCK_FOOTPRINTS / f"{lib}.pretty"
    return base / f"{name}.kicad_mod"


_LIB_ANGLE_CACHE: dict[Path, dict] = {}


def _library_child_angles(lib_path: Path) -> dict:
    """Parse a .kicad_mod library file into {key: relative_angle}.

    key is ("user", text, layer) for `(fp_text user "text" ... (layer
    "layer") ...)`, or ("property", name) for either a modern `(property
    "name" ...)` or an old-style `(fp_text reference ...)`/`(fp_text value
    ...)` (which correspond to the "Reference"/"Value" properties on a
    placed instance). These angles are the item's own, un-rotated, as
    authored in the library -- i.e. relative to the footprint.
    """
    if lib_path in _LIB_ANGLE_CACHE:
        return _LIB_ANGLE_CACHE[lib_path]
    result: dict = {}
    if lib_path.is_file():
        src = lib_path.read_text()
        for m in re.finditer(r'\(fp_text\s+(\w+)\s+"((?:[^"\\]|\\.)*)"', src):
            kind, content = m.group(1), m.group(2)
            item_start, item_end = _find_footprint_block(src, m.start())
            item = src[item_start:item_end]
            at_m = _ANGLE_AT_RE.search(item)
            angle = float(at_m.group(3) or 0) % 360 if at_m else 0.0
            if kind == "user":
                layer_m = re.search(r'\(layer "([^"]+)"\)', item)
                result[("user", content, layer_m.group(1) if layer_m else None)] = angle
            elif kind == "reference":
                result[("property", "Reference")] = angle
            elif kind == "value":
                result[("property", "Value")] = angle
        for m in re.finditer(r'\(property\s+"([^"]+)"', src):
            item_start, item_end = _find_footprint_block(src, m.start())
            item = src[item_start:item_end]
            at_m = _ANGLE_AT_RE.search(item)
            angle = float(at_m.group(3) or 0) % 360 if at_m else 0.0
            result.setdefault(("property", m.group(1)), angle)
    _LIB_ANGLE_CACHE[lib_path] = result
    return result


def _child_lookup_key(kind: str, item: str) -> tuple | None:
    """Return the (kind, ...) lookup key for a placed pad/fp_text/property
    child, matching the keys _library_child_angles() produces."""
    if kind == "fp_text":
        tm = re.match(r'\(fp_text\s+\w+\s+"((?:[^"\\]|\\.)*)"', item)
        if not tm:
            return None
        layer_m = re.search(r'\(layer "([^"]+)"\)', item)
        return ("user", tm.group(1), layer_m.group(1) if layer_m else None)
    if kind == "property":
        nm = re.match(r'\(property\s+"([^"]+)"', item)
        return ("property", nm.group(1)) if nm else None
    return None


def _set_child_angles(block: str, target_rot: float, lib_angles: dict) -> str:
    """Fix up every (pad ...), (fp_text ...) and (property ...) child's
    `(at x y [angle])` inside a footprint block, using two different rules.

    KiCad's board format stores each child's angle as an ABSOLUTE angle
    (the footprint's own rotation plus the item's angle in the library
    copy), not one relative to the footprint, so rotating a footprint does
    not rotate its children's copper/text shapes automatically -- only
    their x/y positions do (those are footprint-local; KiCad reinterprets
    them under the new rotation on its own).

    Pads: every pad on this board has zero relative rotation in its
    library copy, so the correct angle is simply the footprint's target
    rotation -- SET it directly.

    fp_text/property: these often DO have a nonzero relative angle in the
    library copy (e.g. Pico-Carrier's 40 pin-name labels are 45 deg
    relative). The correct absolute angle is `(target_rot + library
    relative angle) mod 360`, looked up from `lib_angles` (see
    _library_child_angles()) -- NOT derived by adding a delta to whatever
    is already on file, which cannot recover a value an earlier bug (or
    manual edit) already got wrong: if the file's own rotation already
    equals the target, a delta of 0 leaves a stale value stale forever.
    An item with no library match is left untouched, with a one-line
    notice (its correct angle isn't determinable from the library).

    Either way, items already at the correct end angle are left untouched
    (including staying absent when the result is 0), so already-correct
    footprints produce no diff noise. The optional trailing `unlocked`
    token (older KiCad output) is preserved.
    """
    target = target_rot % 360
    target_s = _fmt_angle(target)
    out = []
    i = 0
    for m in _ANGLE_ITEM_RE.finditer(block):
        if m.start() < i:
            continue  # inside an already-processed item
        item_start, item_end = _find_footprint_block(block, m.start())
        item = block[item_start:item_end]
        kind = m.group(1)
        at_m = _ANGLE_AT_RE.search(item)
        if at_m:
            x, y, ang, unlocked = at_m.group(1), at_m.group(2), at_m.group(3), (at_m.group(4) or "")
            cur = float(ang or 0) % 360
            if kind == "pad":
                new_ang = target
            else:
                key = _child_lookup_key(kind, item)
                lib_angle = lib_angles.get(key) if key else None
                if lib_angle is None:
                    print(f"note: no library angle for {key or (kind,)}, leaving angle as-is")
                    new_ang = cur
                else:
                    new_ang = (target + lib_angle) % 360
            new_ang_s = _fmt_angle(new_ang)
            if _fmt_angle(cur) != new_ang_s:
                item = item[:at_m.start()] + f'(at {x} {y} {new_ang_s}{unlocked})' + item[at_m.end():]
        out.append(block[i:item_start])
        out.append(item)
        i = item_end
    out.append(block[i:])
    return "".join(out)


def _self_check_child_angles(text: str) -> None:
    """For every footprint, assert every pad's absolute angle equals the
    footprint's own rotation, and every fp_text/property with a resolvable
    library match has angle == (footprint rotation + library relative
    angle) mod 360. Items with no library match aren't checked (their
    correct value isn't determinable from the library either)."""
    for m in re.finditer(r'\(footprint\s+"([^"]+)"', text):
        lib_id = m.group(1)
        fp_start, fp_end = _find_footprint_block(text, m.start())
        block = text[fp_start:fp_end]
        ref_m = re.search(r'\(property\s+"Reference"\s+"([^"]+)"', block)
        ref = ref_m.group(1) if ref_m else "?"
        fp_at = re.search(r'\n\t+\(at\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?\)', block)
        fp_angle = float(fp_at.group(3) or 0) % 360 if fp_at else 0.0
        lib_angles = _library_child_angles(_footprint_lib_path(lib_id)) if _footprint_lib_path(lib_id) else {}
        for cm in _ANGLE_ITEM_RE.finditer(block):
            c_start, c_end = _find_footprint_block(block, cm.start())
            item = block[c_start:c_end]
            kind = cm.group(1)
            at_m = _ANGLE_AT_RE.search(item)
            cur = float(at_m.group(3) or 0) % 360 if at_m and at_m.group(3) else 0.0
            if kind == "pad":
                assert abs(cur - fp_angle) < 1e-6, (
                    f"{ref}: pad angle {cur} != footprint angle {fp_angle}"
                )
            else:
                key = _child_lookup_key(kind, item)
                lib_angle = lib_angles.get(key) if key else None
                if lib_angle is None:
                    continue
                expected = (fp_angle + lib_angle) % 360
                assert abs(cur - expected) < 1e-6, (
                    f"{ref}: {kind} {key} angle {cur} != expected {expected} "
                    f"(footprint {fp_angle} + library {lib_angle})"
                )


def main() -> int:
    text = PCB.read_text()
    # Iterate footprint blocks, rewriting each that has a matching ref
    changes = 0
    seen: set[str] = set()
    out_parts: list[str] = []
    last_end = 0
    for m in re.finditer(r'\(footprint\s+"([^"]+)"', text):
        lib_id = m.group(1)
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
        # The footprint's own rotation is absolute; its pad/text/property
        # children's angles are ALSO absolute in the file (not relative), so
        # they must be fixed up too or their copper/text shapes go stale
        # (see _set_child_angles docstring: pads are SET to the target;
        # text/properties get the library's own relative angle added to the
        # target, looked up by lib_id -- not derived from whatever is
        # already on file, which can't recover a value an earlier bug
        # already flattened). Done for every footprint we place, not just
        # ones whose rotation is changing this run, so this self-heals any
        # pre-existing staleness too.
        lib_path = _footprint_lib_path(lib_id)
        lib_angles = _library_child_angles(lib_path) if lib_path else {}
        block2 = _set_child_angles(block2, rot, lib_angles)
        out_parts.append(text[last_end:fp_start])
        out_parts.append(block2)
        last_end = fp_end
        changes += 1
    out_parts.append(text[last_end:])
    out_text = "".join(out_parts)
    out_text = add_board_extras(out_text)
    _self_check_child_angles(out_text)
    PCB.write_text(out_text)
    print(f"moved {changes} footprints")

    missing = sorted(set(PLACEMENT) - seen)
    for ref in missing:
        print(f"missing: {ref}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
