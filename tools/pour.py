#!/usr/bin/env python3
"""Add (or refresh) the GND pours on both copper layers and fill them.

usage: pour.py BOARD [--out OUT]

Run with KiCad's bundled python3 (needs pcbnew). Zones named GND_F / GND_B are
replaced if present, so the script is idempotent. The pour outline is the board
outline inset 0.3 mm, cut 1 mm above the cartridge fingers (no copper under the
edge connector; the finger area's top is read from P1's rule areas); the antenna
and NPTH keepouts forbid copper where it must not go.
"""
import argparse
import pcbnew

EDGE_INSET_MM = 0.3   # pour stays this far from the board outline (JLC wants >= 0.2)
FINGER_GAP_MM = 1.0   # pour stops this far above the finger area


def finger_top_mm(board):
    """Top of the cartridge-finger area = top of P1's per-finger rule areas."""
    p1 = board.FindFootprintByReference("P1")
    tops = [z.GetBoundingBox().GetTop() for z in p1.Zones() if z.GetIsRuleArea()]
    if not tops:
        raise SystemExit("P1 has no rule areas; cannot locate the finger area")
    return pcbnew.ToMM(min(tops))


def add_pours(board):
    gnd = board.GetNetsByName()["GND"]
    # Outline first: after board.Remove(zone) the SWIG SHAPE_POLY_SET proxy from
    # GetBoardPolygonOutlines loses BBox() (KiCad 10.0.6 quirk).
    outline = pcbnew.SHAPE_POLY_SET()
    if not board.GetBoardPolygonOutlines(outline, True):
        raise SystemExit("could not compute the board outline")
    outline.Deflate(pcbnew.FromMM(EDGE_INSET_MM), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS,
                    int(pcbnew.FromMM(pcbnew.ARC_LOW_DEF_MM)))
    bb = outline.BBox()
    x0, y0, x1 = bb.GetLeft(), bb.GetOrigin().y, bb.GetRight()
    y1 = pcbnew.FromMM(finger_top_mm(board) - FINGER_GAP_MM)
    for z in list(board.Zones()):
        if z.GetZoneName() in ("GND_F", "GND_B"):
            board.Remove(z)
    for layer, name in ((pcbnew.F_Cu, "GND_F"), (pcbnew.B_Cu, "GND_B")):
        z = pcbnew.ZONE(board)
        z.SetZoneName(name)
        z.SetLayer(layer)
        z.SetNet(gnd)
        z.SetLocalClearance(pcbnew.FromMM(0.25))
        z.SetMinThickness(pcbnew.FromMM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(pcbnew.FromMM(0.3))
        z.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.4))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        o = z.Outline()  # edit the zone's own polygon: SetOutline() hands over ownership and crashes
        o.NewOutline()
        for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            o.Append(x, y)
        o.BooleanIntersection(outline)  # inset outline, cut above the fingers
        board.Add(z)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--out")
    a = ap.parse_args()
    b = pcbnew.LoadBoard(a.board)
    add_pours(b)
    b.Save(a.out or a.board)
    print("pours added and filled:", a.out or a.board)


if __name__ == "__main__":
    main()
