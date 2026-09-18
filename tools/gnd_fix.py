#!/usr/bin/env python3
"""Close the GND net after pouring: route every stray GND cluster (pads, vias,
tracks, pour islands) to the main cluster with the grid router, refill, repeat;
then give starved-thermal pads a track connection.

usage: gnd_fix.py BOARD [--out OUT] [--drc DRC_JSON]   (KiCad's bundled python3)
--drc: a kicad-cli DRC json of BOARD; its starved_thermal pads get a track each.
Run after pour.py (it refills the pours itself). Exit status 1 if a cluster
could not be linked (the board is still saved).
"""
import argparse
import json
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grid_route  # noqa: E402

LAYERS = grid_route.LAYERS
mm = lambda v: v / 1e6  # noqa: E731


class UF:
    def __init__(self):
        self.p = {}

    def find(self, a):
        self.p.setdefault(a, a)
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def union(self, a, b):
        self.p[self.find(a)] = self.find(b)


def gnd_clusters(board, gc):
    """Return list of clusters; each cluster is a list of (pos VECTOR2I, layers frozenset)."""
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.BuildConnectivity()
    conn = board.GetConnectivity()
    items = []  # (key, obj, anchors)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetCode() == gc:
                layers = frozenset(l for l in LAYERS if pad.IsOnLayer(l))
                items.append((("pad", id(pad)), pad, [(pad.GetPosition(), layers)]))
    for t in board.GetTracks():
        if t.GetNetCode() != gc:
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            items.append((("via", id(t)), t, [(t.GetPosition(), frozenset(LAYERS))]))
        else:
            L = frozenset([t.GetLayer()])
            items.append((("trk", id(t)), t, [(t.GetStart(), L), (t.GetEnd(), L)]))
    uf = UF()
    by_obj = {}
    for key, obj, _ in items:
        uf.find(key)
        by_obj[id(obj)] = key
    for key, obj, _ in items:
        for other in conn.GetConnectedItems(obj):
            if isinstance(other, pcbnew.ZONE):  # one ZONE object spans all its islands; handled below
                continue
            ok = by_obj.get(id(other))
            if ok is None:  # SWIG may hand back a fresh proxy: match by position+type
                for k2, o2, _ in items:
                    if type(o2) is type(other) and o2.GetPosition() == other.GetPosition() \
                            and (not isinstance(o2, pcbnew.PCB_TRACK) or isinstance(o2, pcbnew.PCB_VIA)
                                 or o2.GetEnd() == other.GetEnd()):
                        ok = k2
                        break
            if ok is not None:
                uf.union(key, ok)
    # pour islands
    islands = []  # (key, poly, layer)
    for z in board.Zones():
        if z.GetNetCode() != gc or z.GetIsRuleArea():
            continue
        for layer in LAYERS:
            if not z.IsOnLayer(layer):
                continue
            polys = z.GetFilledPolysList(layer)
            for i in range(polys.OutlineCount()):
                isl = pcbnew.SHAPE_POLY_SET()
                isl.AddOutline(polys.Outline(i))
                key = ("isl", id(z), layer, i)
                uf.find(key)
                islands.append((key, isl, layer))
    for key, obj, anchors in items:
        for ikey, isl, layer in islands:
            on = any(layer in a[1] for a in anchors)
            if not on:
                continue
            shape = obj.GetEffectiveShape(layer) if isinstance(obj, (pcbnew.PAD, pcbnew.PCB_VIA)) \
                else obj.GetEffectiveShape()
            if isl.Collide(shape, int(pcbnew.FromMM(0.05))):  # spokes/ends only touch the fill
                uf.union(key, ikey)
    clusters = {}
    for key, obj, anchors in items:
        clusters.setdefault(uf.find(key), []).extend(anchors)
    for ikey, isl, layer in islands:
        clusters.setdefault(uf.find(ikey), []).extend(
            (pt, frozenset([layer])) for pt in interior_points(isl))
    return sorted(clusters.values(), key=len, reverse=True)


def interior_points(isl, pitch_mm=0.6, inset_mm=0.3):
    """Grid points safely inside a pour island: a track may end on any of them."""
    bb = isl.BBox()
    step, ins = pcbnew.FromMM(pitch_mm), pcbnew.FromMM(inset_mm)
    out = []
    y = bb.GetTop() + step // 2
    while y < bb.GetBottom():
        x = bb.GetLeft() + step // 2
        while x < bb.GetRight():
            if all(isl.Contains(pcbnew.VECTOR2I(x + dx, y + dy))
                   for dx, dy in ((0, 0), (ins, 0), (-ins, 0), (0, ins), (0, -ins))):
                out.append(pcbnew.VECTOR2I(x, y))
            x += step
        y += step
    return out


def make_router(board):
    nc = board.GetAllNetClasses()["Default"]
    ds = board.GetDesignSettings()
    idx = grid_route.build_static_obstacles(board)
    keep = grid_route.Keepouts(board)
    region = grid_route.build_board_region(board, pcbnew.FromMM(0.3))
    # GND stitching vias use the board's legal minimum (0.6/0.3 mm; min via 0.5, min
    # hole 0.3 in the design rules) rather than the 0.8/0.4 netclass default: the
    # pockets that need them sit between 1.27 mm-pitch SOIC rows where 0.8 won't fit.
    return grid_route.Router(board, idx, keep, region, nc.GetTrackWidth(), pcbnew.FromMM(0.6),
                             pcbnew.FromMM(0.3), nc.GetClearance(), ds.m_HoleClearance)


def nearest_pair(a, b):
    best = None
    for pa, la in a:
        for pb, lb in b:
            d = (pa - pb).EuclideanNorm()
            if best is None or d < best[0]:
                best = (d, (pa, la), (pb, lb))
    return best


def route(router, gc, src, dst, name):
    for margin, vs, grid in ((8.0, 1.0, 0.2), (20.0, 0.5, 0.2), (None, 0.3, 0.2)):
        res = router.route_net(gc, name, src, dst, via_cost_scale=vs, margin_mm=margin, grid_mm=grid)
        if res is not None:
            router.commit(res[0], gc)
            return res
    return None


def close_clusters(board, gc, max_rounds=8):
    for rnd in range(max_rounds):
        cl = gnd_clusters(board, gc)
        print(f"round {rnd}: {len(cl)} GND clusters")
        if len(cl) == 1:
            return True
        router = make_router(board)
        main = cl[0]
        done = 0
        for stray in cl[1:]:
            d, s, t = nearest_pair(stray, main)
            res = route(router, gc, s, t, "GND")
            if res:
                done += 1
                print(f"  linked cluster of {len(stray)} anchors: {mm(res[1]):.1f} mm, {res[2]} vias")
            else:
                print(f"  FAILED to link cluster of {len(stray)} anchors near ({mm(s[0].x):.1f},{mm(s[0].y):.1f})")
        if done == 0:
            return False
    return False


def fix_starved(board, gc, drc_json):
    d = json.load(open(drc_json))
    pads = []
    for v in d["violations"]:
        if v["type"] != "starved_thermal":
            continue
        for it in v["items"]:
            if it["description"].startswith(("Pad", "PTH pad")):
                pads.append((it["pos"]["x"], it["pos"]["y"]))
    if not pads:
        return
    cl = gnd_clusters(board, gc)
    router = make_router(board)
    for x, y in pads:
        p = pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))
        src = grid_route.find_anchor(board, gc, x, y)
        # target: nearest other anchor of the main cluster at least 1.5 mm away
        cands = [(pt, L) for pt, L in cl[0] if (pt - p).EuclideanNorm() > pcbnew.FromMM(1.5)]
        best = nearest_pair([src], cands)
        if best is None:
            print(f"  starved pad ({x},{y}): no main-cluster anchor to route to")
            continue
        d, s, t = best
        res = route(router, gc, s, t, "GND")
        print(f"  starved pad ({x},{y}): {'linked %.1f mm' % mm(res[1]) if res else 'FAILED'}")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--out")
    ap.add_argument("--drc")
    a = ap.parse_args()
    b = pcbnew.LoadBoard(a.board)
    gc = b.GetNetsByName()["GND"].GetNetCode()
    ok = close_clusters(b, gc)
    if a.drc:
        fix_starved(b, gc, a.drc)
    else:
        print("note: without --drc DRC_JSON starved-thermal pads are not fixed; run DRC and pass it")
    b.Save(a.out or a.board)
    print("saved", a.out or a.board, "| clusters closed:", ok)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
