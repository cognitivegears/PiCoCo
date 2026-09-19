#!/usr/bin/env python3
"""GND stitching after finish_route.py: (1) a via next to every GND pad of a
decoupling capacitor and every IC GND pin (the board's own "each bypass cap gets a
via straight to the plane" rule), (2) a stitching grid tying the F.Cu and B.Cu GND
fills together wherever both fills exist. Then refill and report.

usage: gnd_stitch.py BOARD [--out OUT] [--pitch MM]   (KiCad's bundled python3)

Vias are only dropped where grid_route's legality test passes (netclass clearance,
hole-to-hole, rule areas) AND both GND fills contain the point, so they never dangle.
The per-pad via is joined to its pad with a short 0.3 mm track when the fill does
not reach the pad (thermal reliefs are kept).
"""
import argparse
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grid_route  # noqa: E402

NOBODY = -999
mm = lambda v: v / 1e6  # noqa: E731


def fills(board, gc):
    out = {}
    for z in board.Zones():
        if z.GetIsRuleArea() or z.GetNetCode() != gc:
            continue
        for L in grid_route.LAYERS:
            if z.IsOnLayer(L):
                out[L] = z.GetFilledPolysList(L)
    return out


def add_via(board, router, gnd, p, dia_mm, drill_mm):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(p)
    v.SetDrill(pcbnew.FromMM(drill_mm))
    v.SetWidth(pcbnew.FromMM(dia_mm))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(gnd)
    board.Add(v)
    for L in grid_route.LAYERS:
        router.idx.add_copper(L, v.GetEffectiveShape(L), gnd.GetNetCode())
    router.idx.add_hole(v.GetEffectiveHoleShape(), gnd.GetNetCode())
    router.edge_cache.clear()
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--out")
    ap.add_argument("--pitch", type=float, default=8.0)
    a = ap.parse_args()
    board = pcbnew.LoadBoard(a.board)
    gnd = board.GetNetsByName()["GND"]
    gc = gnd.GetNetCode()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    nc = board.GetAllNetClasses()["Default"]
    ds = board.GetDesignSettings()
    idx = grid_route.build_static_obstacles(board)
    keep = grid_route.Keepouts(board)
    region = grid_route.build_board_region(board, pcbnew.FromMM(0.5))
    router = grid_route.Router(board, idx, keep, region, pcbnew.FromMM(0.3), pcbnew.FromMM(0.6),
                               pcbnew.FromMM(0.3), nc.GetClearance(), ds.m_HoleClearance)
    fl = fills(board, gc)

    def both_filled(p):
        return all(L in fl and fl[L].Contains(p) for L in grid_route.LAYERS)

    # 1) per-pad vias
    done = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if not (ref.startswith("C") or ref.startswith("U")) or ref in ("U1",) or fp.IsDNP():
            continue
        for pad in fp.Pads():
            if pad.GetNetCode() != gc or not pad.IsOnLayer(pcbnew.F_Cu):
                continue
            c = pad.GetPosition()
            best = None
            for r_mm in (1.0, 1.2, 1.4, 1.6, 1.9, 2.2, 2.6):
                for k in range(16):
                    ang = k * math.pi / 8
                    p = pcbnew.VECTOR2I(int(c.x + pcbnew.FromMM(r_mm) * math.cos(ang)),
                                        int(c.y + pcbnew.FromMM(r_mm) * math.sin(ang)))
                    if not router.in_bounds(p.x, p.y) or not router.via_ok((p.x, p.y), NOBODY):
                        continue
                    if not router.edge_ok_track((c.x, c.y), (p.x, p.y), pcbnew.F_Cu, gc):
                        continue
                    best = p
                    break
                if best:
                    break
            if best is None:
                print(f"  no via spot for {ref} pad {pad.GetNumber()}")
                continue
            add_via(board, router, gnd, best, 0.6, 0.3)
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(c)
            t.SetEnd(best)
            t.SetWidth(pcbnew.FromMM(0.3))
            t.SetLayer(pcbnew.F_Cu)
            t.SetNet(gnd)
            board.Add(t)
            router.idx.add_copper(pcbnew.F_Cu, t.GetEffectiveShape(), gc)
            done.append(f"{ref}.{pad.GetNumber()}")
    print("per-pad GND vias:", len(done), done)

    # 2) stitching grid where both fills exist
    x0, y0, x1, y1 = region[1]
    step = pcbnew.FromMM(a.pitch)
    n = 0
    y = y0 + step // 2
    while y < y1:
        x = x0 + step // 2
        while x < x1:
            p = pcbnew.VECTOR2I(x, y)
            if router.in_bounds(x, y) and both_filled(p) and router.via_ok((x, y), NOBODY):
                add_via(board, router, gnd, p, 0.6, 0.3)
                n += 1
            x += step
        y += step
    print("stitching vias:", n)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(a.out or a.board)
    print("saved", a.out or a.board)


if __name__ == "__main__":
    main()
