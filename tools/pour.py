#!/usr/bin/env python3
"""Add (or refresh) the GND pours on both copper layers and fill them.

usage: pour.py BOARD [--out OUT]

Run with KiCad's bundled python3 (needs pcbnew). Zones named GND_F / GND_B are
replaced if present, so the script is idempotent. The pour stops 1 mm above the
cartridge fingers (no copper under the edge connector) and the antenna / NPTH
keepouts already forbid copper where it must not go.
"""
import argparse
import pcbnew

FINGER_TOP_MM = 99.7  # y where the finger area starts (board outline notch)


def add_pours(board):
    gnd = board.GetNetsByName()["GND"]
    for z in list(board.Zones()):
        if z.GetZoneName() in ("GND_F", "GND_B"):
            board.Remove(z)
    bb = board.GetBoardEdgesBoundingBox()
    x0, y0, x1 = bb.GetLeft(), bb.GetTop(), bb.GetRight()
    y1 = pcbnew.FromMM(FINGER_TOP_MM - 1.0)
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
        o = z.Outline()
        o.NewOutline()
        for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            o.Append(x, y)
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
