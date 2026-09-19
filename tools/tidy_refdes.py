#!/usr/bin/env python3
"""Put every reference designator just above (or, for the buffer row, below) its
footprint's courtyard, upright, so a hand-builder can read which part is which.
usage: tidy_refdes.py BOARD [--out OUT]   (KiCad's bundled python3)
Skips U1, P1 and the fiducials; a text that would land inside another part's
courtyard is placed on the opposite side instead."""
import argparse
import pcbnew

SKIP = ("U1", "P1")
TOPRIGHT = ("U10", "U11", "U12", "U13")  # decoupling cap sits above the centre; bottom is the board edge
BELOW = ("C10",)  # legend above it
HIDE = ("P1", "FID1", "FID2", "FID3")  # no use to a builder; they only collide


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--out")
    a = ap.parse_args()
    b = pcbnew.LoadBoard(a.board)
    for f in b.GetFootprints():
        if f.GetReference() in HIDE:
            f.Reference().SetVisible(False)
    fps = [f for f in b.GetFootprints() if f.GetReference() not in SKIP and not f.GetReference().startswith("FID")]
    boxes = {f.GetReference(): f.GetCourtyard(pcbnew.F_CrtYd).BBox() for f in fps}
    h = pcbnew.FromMM(0.8)
    for f in fps:
        r = f.GetReference()
        bb = boxes[r]
        ref = f.Reference()
        ref.SetTextSize(pcbnew.VECTOR2I(h, h))
        ref.SetTextThickness(pcbnew.FromMM(0.12))
        ref.SetVisible(True)
        cx = bb.GetCenter().x
        above = pcbnew.VECTOR2I(cx, bb.GetTop() - h)
        below = pcbnew.VECTOR2I(cx, bb.GetBottom() + h)
        topright = pcbnew.VECTOR2I(bb.GetRight() - pcbnew.FromMM(1.5), bb.GetTop() - h)
        pref = [topright, above, below] if r in TOPRIGHT else [below, above] if r in BELOW else [above, below]
        pos = pref[0]
        for cand in pref:
            if not any(o != r and boxes[o].Contains(cand) for o in boxes):
                pos = cand
                break
        ref.SetPosition(pos)
        ref.SetTextAngleDegrees(0)
        ref.SetLayer(pcbnew.F_SilkS)
    b.Save(a.out or a.board)
    print("refdes tidied:", len(fps))


if __name__ == "__main__":
    main()
