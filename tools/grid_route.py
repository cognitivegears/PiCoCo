#!/usr/bin/env python3
"""Grid A* router for finishing the last unrouted nets on a KiCad board.

Must run under KiCad's bundled python3 (it imports pcbnew):
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

Usage:
  grid_route.py BOARD [--out OUT] [--net NAME ...]

Routes the given nets on a 0.2 mm grid using A* (8-connected, layer change =
via), one net at a time, shortest first, adding each finished net's copper to
the obstacle set before the next. Legality of every step is a real pcbnew
SHAPE collision test against every other-net pad/track/via on that layer
(netclass clearance), every hole (board hole clearance), rule areas that
forbid tracks/vias, and the board outline eroded by 0.3 mm. Nets are named by
--net (using the HINTS table) or by --pair NET X1 Y1 X2 Y2 (mm), which routes
between the existing items of NET nearest those two points. Written for the
v2.3 main board after Freerouting left 7 nets (docs/kicad-workflow.md).
"""
import argparse
import heapq
import math
import sys
import time
from collections import defaultdict

import pcbnew

F_CU = pcbnew.F_Cu
B_CU = pcbnew.B_Cu
LAYERS = (F_CU, B_CU)

BUCKET = 1_000_000  # 1 mm, in IU (nm)
DIAG = math.sqrt(2)
VIA_COST_MM = 3.0  # brief: layer change costs like ~3 mm of travel


def bucket_range(bbox, margin):
    lo_x = (bbox.GetLeft() - margin) // BUCKET
    hi_x = (bbox.GetRight() + margin) // BUCKET
    lo_y = (bbox.GetTop() - margin) // BUCKET
    hi_y = (bbox.GetBottom() + margin) // BUCKET
    return range(lo_x, hi_x + 1), range(lo_y, hi_y + 1)


class ObstacleIndex:
    """Bucket-grid spatial index of copper shapes (per layer) and hole shapes."""

    def __init__(self):
        self.copper = {F_CU: defaultdict(list), B_CU: defaultdict(list)}
        self.holes = defaultdict(list)

    def add_copper(self, layer, shape, netcode):
        bbox = shape.BBox()
        xs, ys = bucket_range(bbox, 0)
        entry = (shape, netcode)
        d = self.copper[layer]
        for bx in xs:
            for by in ys:
                d[(bx, by)].append(entry)

    def add_hole(self, shape, netcode):
        bbox = shape.BBox()
        xs, ys = bucket_range(bbox, 0)
        entry = (shape, netcode)
        for bx in xs:
            for by in ys:
                self.holes[(bx, by)].append(entry)

    def _nearby(self, table, shape, margin):
        bbox = shape.BBox()
        xs, ys = bucket_range(bbox, margin)
        seen = set()
        out = []
        for bx in xs:
            for by in ys:
                for entry in table.get((bx, by), ()):
                    key = id(entry[0])
                    if key not in seen:
                        seen.add(key)
                        out.append(entry)
        return out

    def copper_blocked(self, layer, shape, netcode, clearance):
        for other_shape, other_net in self._nearby(self.copper[layer], shape, clearance):
            if other_net == netcode:
                continue
            if pcbnew.SHAPE.Collide(shape, other_shape, clearance):
                return True
        return False

    def hole_blocked(self, shape, netcode, hole_clearance):
        for other_shape, other_net in self._nearby(self.holes, shape, hole_clearance):
            if other_net == netcode:
                continue
            if pcbnew.SHAPE.Collide(shape, other_shape, hole_clearance):
                return True
        return False


class Keepouts:
    """Rule-area zones that forbid tracks/vias, read from the board (not hardcoded)."""

    def __init__(self, board):
        self.areas = []  # (outline_polyset, layer_set, forbid_tracks, forbid_vias)
        # board.Zones() only returns board-owned zones; footprints (e.g. the
        # cartridge-edge connector's per-finger keepouts) own their own zones.
        zone_lists = [board.Zones()] + [fp.Zones() for fp in board.GetFootprints()]
        for zone_list in zone_lists:
            for z in zone_list:
                if not z.GetIsRuleArea():
                    continue
                forbid_t = z.GetDoNotAllowTracks()
                forbid_v = z.GetDoNotAllowVias()
                if not (forbid_t or forbid_v):
                    continue
                self.areas.append((z.GetZoneName(), z.Outline(), z.GetLayerSet(), forbid_t, forbid_v))

    def blocks_track(self, layer, shape):
        for name, outline, layerset, forbid_t, forbid_v in self.areas:
            if not forbid_t or not layerset.Contains(layer):
                continue
            if outline.Collide(shape, 0):
                return True
        return False

    def blocks_via(self, shape_f, shape_b):
        for name, outline, layerset, forbid_t, forbid_v in self.areas:
            if not forbid_v:
                continue
            if layerset.Contains(F_CU) and outline.Collide(shape_f, 0):
                return True
            if layerset.Contains(B_CU) and outline.Collide(shape_b, 0):
                return True
        return False


def build_static_obstacles(board):
    idx = ObstacleIndex()
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            net = pad.GetNetCode()
            for layer in LAYERS:
                if pad.IsOnLayer(layer):
                    idx.add_copper(layer, pad.GetEffectiveShape(layer), net)
            if pad.HasHole():
                idx.add_hole(pad.GetEffectiveHoleShape(), net)
    for trk in board.GetTracks():
        net = trk.GetNetCode()
        if isinstance(trk, pcbnew.PCB_VIA):
            for layer in LAYERS:
                idx.add_copper(layer, trk.GetEffectiveShape(layer), net)
            idx.add_hole(trk.GetEffectiveHoleShape(), net)
        else:
            idx.add_copper(trk.GetLayer(), trk.GetEffectiveShape(), net)
    return idx


def build_board_region(board, edge_clearance):
    """The board outline (may be non-rectangular, e.g. the cartridge-edge
    tongue), eroded inward by edge_clearance. Routing must stay inside this."""
    outlines = pcbnew.SHAPE_POLY_SET()
    ok = board.GetBoardPolygonOutlines(outlines, True)
    if not ok:
        raise SystemExit("could not compute board outline polygon")
    max_err = int(round(pcbnew.FromMM(pcbnew.ARC_LOW_DEF_MM)))
    outlines.Deflate(edge_clearance, pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, max_err)
    bb = outlines.BBox()
    box = (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom())
    return outlines, box


def find_anchor(board, netcode, x_mm, y_mm):
    """Nearest connectable point (pad center or track/via endpoint) of a net to
    a hinted coordinate. Returns (VECTOR2I pos, allowed_layers set)."""
    target = pcbnew.VECTOR2I(pcbnew.FromMM(x_mm), pcbnew.FromMM(y_mm))
    best = None
    best_d = None

    def consider(pos, layers):
        nonlocal best, best_d
        d = (pos - target).EuclideanNorm()
        if best_d is None or d < best_d:
            best_d = d
            best = (pos, layers)

    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetCode() != netcode:
                continue
            layers = frozenset(l for l in LAYERS if pad.IsOnLayer(l))
            consider(pad.GetPosition(), layers)
    for trk in board.GetTracks():
        if trk.GetNetCode() != netcode:
            continue
        if isinstance(trk, pcbnew.PCB_VIA):
            consider(trk.GetPosition(), frozenset(LAYERS))
        else:
            layers = frozenset([trk.GetLayer()])
            consider(trk.GetStart(), layers)
            consider(trk.GetEnd(), layers)
    if best is None:
        raise SystemExit(f"no existing item found for net {netcode}")
    return best


def octile(dx, dy):
    dx, dy = abs(dx), abs(dy)
    return (dx + dy) + (DIAG - 2) * min(dx, dy)


class Router:
    def __init__(self, board, static_idx, keepouts, region, track_w, via_dia, via_drill,
                 clearance, hole_clearance):
        self.board = board
        self.idx = static_idx
        self.keepouts = keepouts
        self.region_poly, self.region_bbox = region
        self.w = track_w
        self.via_dia = via_dia
        self.via_drill = via_drill
        self.clearance = clearance
        self.hole_clearance = hole_clearance
        self.via_cost = int(round(pcbnew.FromMM(VIA_COST_MM)))
        self.edge_cache = {}
        self.in_bounds_cache = {}
        self.max_expansions = 400_000

    def in_bounds(self, x, y, domain=None):
        b = self.region_bbox
        if not (b[0] <= x <= b[2] and b[1] <= y <= b[3]):
            return False
        if domain is not None and not (domain[0] <= x <= domain[2] and domain[1] <= y <= domain[3]):
            return False
        key = (x, y)
        v = self.in_bounds_cache.get(key)
        if v is None:
            v = self.region_poly.Contains(pcbnew.VECTOR2I(x, y))
            self.in_bounds_cache[key] = v
        return v

    def edge_ok_track(self, p, q, layer, netcode):
        key = (p, q, layer, netcode, "t")
        v = self.edge_cache.get(key)
        if v is not None:
            return v
        shape = pcbnew.SHAPE_SEGMENT(pcbnew.VECTOR2I(*p), pcbnew.VECTOR2I(*q), self.w)
        ok = True
        if self.keepouts.blocks_track(layer, shape):
            ok = False
        elif self.idx.copper_blocked(layer, shape, netcode, self.clearance):
            ok = False
        elif self.idx.hole_blocked(shape, netcode, self.hole_clearance):
            ok = False
        self.edge_cache[key] = ok
        return ok

    def via_ok(self, p, netcode):
        key = (p, netcode, "v")
        v = self.edge_cache.get(key)
        if v is not None:
            return v
        pt = pcbnew.VECTOR2I(*p)
        cu_shape = pcbnew.SHAPE_CIRCLE(pt, self.via_dia // 2)
        hole_shape = pcbnew.SHAPE_CIRCLE(pt, self.via_drill // 2)
        ok = True
        if self.keepouts.blocks_via(cu_shape, cu_shape):
            ok = False
        elif self.idx.copper_blocked(F_CU, cu_shape, netcode, self.clearance):
            ok = False
        elif self.idx.copper_blocked(B_CU, cu_shape, netcode, self.clearance):
            ok = False
        elif self.idx.hole_blocked(hole_shape, netcode, self.hole_clearance):
            ok = False
        self.edge_cache[key] = ok
        return ok

    def route_net(self, netcode, net_name, src, dst, via_cost_scale=1.0, margin_mm=None,
                  grid_mm=0.2):
        """src/dst: (VECTOR2I pos, allowed_layers set). Returns list of
        (kind, ...) drawing ops, or None if no path found."""
        src_pos, src_layers = src
        dst_pos, dst_layers = dst

        GRID = int(round(pcbnew.FromMM(grid_mm)))

        domain = None
        if margin_mm is not None:
            margin = int(round(pcbnew.FromMM(margin_mm)))
            domain = (
                min(src_pos.x, dst_pos.x) - margin,
                min(src_pos.y, dst_pos.y) - margin,
                max(src_pos.x, dst_pos.x) + margin,
                max(src_pos.y, dst_pos.y) + margin,
            )

        def snap(v):
            return int(round(v / GRID)) * GRID

        def nearest_grid_layers(pos, layers):
            gx, gy = snap(pos.x), snap(pos.y)
            options = []
            for layer in layers:
                options.append(((gx, gy, layer)))
            return gx, gy, options

        src_gx, src_gy, src_layer_opts = nearest_grid_layers(src_pos, src_layers)
        dst_gx, dst_gy, dst_layer_opts = nearest_grid_layers(dst_pos, dst_layers)
        dst_nodes = {(dst_gx, dst_gy, layer) for layer in dst_layers}

        vcost = int(round(self.via_cost * via_cost_scale))

        def h(node):
            gx, gy, _ = node
            return octile((gx - dst_gx), (gy - dst_gy))

        open_heap = []
        g_score = {}
        came_from = {}
        start_nodes = []
        for layer in src_layers:
            n = (src_gx, src_gy, layer)
            g_score[n] = 0
            heapq.heappush(open_heap, (h(n), 0, n))
            start_nodes.append(n)

        DIRS = [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
        closed = set()
        goal_node = None
        expansions = 0

        while open_heap:
            f, g, node = heapq.heappop(open_heap)
            if node in closed:
                continue
            if node in dst_nodes:
                goal_node = node
                break
            closed.add(node)
            expansions += 1
            if expansions % 20000 == 0:
                print(f"  ...{net_name}: {expansions} expansions, open={len(open_heap)}, "
                      f"best_f={f}", file=sys.stderr)
            if expansions > self.max_expansions:
                print(f"  {net_name}: giving up after {expansions} expansions", file=sys.stderr)
                return None
            gx, gy, layer = node
            px, py = gx, gy
            if not self.in_bounds(px, py, domain):
                continue
            for dxg, dyg in DIRS:
                nx, ny = gx + dxg * GRID, gy + dyg * GRID
                if not self.in_bounds(nx, ny, domain):
                    continue
                nnode = (nx, ny, layer)
                if nnode in closed:
                    continue
                if not self.edge_ok_track((px, py), (nx, ny), layer, netcode):
                    continue
                step = GRID * (DIAG if dxg and dyg else 1)
                ng = g + step
                if ng < g_score.get(nnode, math.inf):
                    g_score[nnode] = ng
                    came_from[nnode] = node
                    heapq.heappush(open_heap, (ng + h(nnode), ng, nnode))
            # layer change (via) at same point
            other_layer = B_CU if layer == F_CU else F_CU
            vnode = (gx, gy, other_layer)
            if vnode not in closed and self.via_ok((px, py), netcode):
                ng = g + vcost
                if ng < g_score.get(vnode, math.inf):
                    g_score[vnode] = ng
                    came_from[vnode] = ("via", node)
                    heapq.heappush(open_heap, (ng + h(vnode), ng, vnode))

        if goal_node is None:
            return None

        # reconstruct path of nodes/vias
        path = [goal_node]
        cur = goal_node
        vias_at = set()
        while cur not in start_nodes:
            prev = came_from[cur]
            if isinstance(prev, tuple) and prev and prev[0] == "via":
                prev_node = prev[1]
                vias_at.add((cur[0], cur[1]))
                path.append(prev_node)
                cur = prev_node
            else:
                path.append(prev)
                cur = prev
        path.reverse()

        # merge consecutive nodes on same layer into polyline points, emit segments
        ops = []
        length = 0
        # stub from exact src point to first grid node
        first = path[0]
        first_pt = (first[0], first[1])
        src_pt = (src_pos.x, src_pos.y)
        if src_pt != first_pt:
            ops.append(("track", src_pt, first_pt, first[2]))
            length += math.hypot(src_pt[0] - first_pt[0], src_pt[1] - first_pt[1])

        i = 0
        while i < len(path) - 1:
            a = path[i]
            b = path[i + 1]
            if a[2] != b[2]:
                # via between a and b (same x,y)
                ops.append(("via", (a[0], a[1])))
                i += 1
                continue
            ops.append(("track", (a[0], a[1]), (b[0], b[1]), a[2]))
            length += math.hypot(a[0] - b[0], a[1] - b[1])
            i += 1

        last = path[-1]
        last_pt = (last[0], last[1])
        dst_pt = (dst_pos.x, dst_pos.y)
        if dst_pt != last_pt:
            ops.append(("track", last_pt, dst_pt, last[2]))
            length += math.hypot(dst_pt[0] - last_pt[0], dst_pt[1] - last_pt[1])

        # merge collinear consecutive track ops on the same layer
        merged = []
        for op in ops:
            if op[0] == "track" and merged and merged[-1][0] == "track" and merged[-1][3] == op[3] \
                    and merged[-1][2] == op[1]:
                a0 = merged[-1][1]
                a1 = merged[-1][2]
                b1 = op[2]
                # collinear check
                v1 = (a1[0] - a0[0], a1[1] - a0[1])
                v2 = (b1[0] - a1[0], b1[1] - a1[1])
                cross = v1[0] * v2[1] - v1[1] * v2[0]
                if cross == 0:
                    merged[-1] = ("track", a0, b1, op[3])
                    continue
            merged.append(op)

        return merged, length, len(vias_at)

    def commit(self, ops, netcode):
        """Add ops to the board and to the obstacle index."""
        for op in ops:
            if op[0] == "track":
                _, p, q, layer = op
                t = pcbnew.PCB_TRACK(self.board)
                t.SetStart(pcbnew.VECTOR2I(*p))
                t.SetEnd(pcbnew.VECTOR2I(*q))
                t.SetWidth(self.w)
                t.SetLayer(layer)
                t.SetNetCode(netcode)
                self.board.Add(t)
                self.idx.add_copper(layer, t.GetEffectiveShape(), netcode)
            else:
                _, p = op
                v = pcbnew.PCB_VIA(self.board)
                v.SetPosition(pcbnew.VECTOR2I(*p))
                v.SetDrill(self.via_drill)
                v.SetWidth(self.via_dia)
                v.SetLayerPair(F_CU, B_CU)
                v.SetNetCode(netcode)
                self.board.Add(v)
                for layer in LAYERS:
                    self.idx.add_copper(layer, v.GetEffectiveShape(layer), netcode)
                self.idx.add_hole(v.GetEffectiveHoleShape(), netcode)
        self.edge_cache.clear()


# Endpoint hints from BRIEF.md's DRC ratsnest report (net -> two (x_mm, y_mm)
# coordinates). find_anchor() resolves each to the exact nearest existing pad
# or track/via endpoint of that net, so small rounding in the hints is fine.
HINTS = {
    "SLENB_BUF": ((144.96, 58.74), (186.65, 92.64)),
    "E_BUF": ((158.70, 62.00), (147.50, 61.28)),
    "AUDIO_PWM": ((161.30, 62.00), (144.96, 53.66)),
    "PICO_RUN": ((126.37, 47.31), (190.91, 84.00)),
    "PICO_P34": ((116.21, 47.31), (160.00, 62.00)),
    "A11_BUF": ((139.07, 47.31), (172.65, 92.64)),
    "A6_BUF": ((146.69, 65.09), (158.65, 96.44)),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--out")
    ap.add_argument("--net", action="append", default=None)
    ap.add_argument("--pair", nargs=5, action="append", metavar=("NET", "X1", "Y1", "X2", "Y2"),
                    help="route NET between the items nearest (X1,Y1) and (X2,Y2) in mm; repeatable")
    args = ap.parse_args()
    for net, x1, y1, x2, y2 in args.pair or ():
        HINTS[net] = ((float(x1), float(y1)), (float(x2), float(y2)))
        args.net = (args.net or []) + [net]

    t0 = time.time()
    board = pcbnew.LoadBoard(args.board)
    ds = board.GetDesignSettings()
    ncs = board.GetAllNetClasses()
    default_nc = ncs["Default"]

    track_w = default_nc.GetTrackWidth()
    via_dia = default_nc.GetViaDiameter()
    via_drill = default_nc.GetViaDrill()
    clearance = default_nc.GetClearance()
    hole_clearance = ds.m_HoleClearance
    edge_clearance = pcbnew.FromMM(0.3)

    net_names = args.net if args.net else list(HINTS.keys())

    print(f"track_w={pcbnew.ToMM(track_w)}mm via_dia={pcbnew.ToMM(via_dia)}mm "
          f"via_drill={pcbnew.ToMM(via_drill)}mm clearance={pcbnew.ToMM(clearance)}mm "
          f"hole_clearance={pcbnew.ToMM(hole_clearance)}mm")

    static_idx = build_static_obstacles(board)
    keepouts = Keepouts(board)
    region = build_board_region(board, edge_clearance)

    router = Router(board, static_idx, keepouts, region, track_w, via_dia, via_drill,
                     clearance, hole_clearance)

    jobs = []
    for name in net_names:
        nc = board.GetNetcodeFromNetname(name)
        if nc < 0:
            print(f"WARNING: net {name} not found on board, skipping")
            continue
        (ax, ay) = HINTS[name][0]
        (bx, by) = HINTS[name][1]
        src = find_anchor(board, nc, ax, ay)
        dst = find_anchor(board, nc, bx, by)
        dist = (src[0] - dst[0]).EuclideanNorm()
        jobs.append((dist, name, nc, src, dst))

    jobs.sort(key=lambda j: j[0])

    results = []
    for dist, name, nc, src, dst in jobs:
        t1 = time.time()
        res = None
        attempts = [
            (8.0, 1.0, 0.2), (20.0, 1.0, 0.2), (None, 0.3, 0.2),
            (None, 0.3, 0.1),  # finer grid: catches narrow legal gaps a 0.2mm
                               # grid can straddle past, at higher cost
        ]
        for margin_mm, via_scale, grid_mm in attempts:
            res = router.route_net(nc, name, src, dst, via_cost_scale=via_scale,
                                    margin_mm=margin_mm, grid_mm=grid_mm)
            if res is not None:
                break
        if res is None:
            results.append((name, None, None, None, time.time() - t1))
            print(f"FAILED to route {name}")
            continue
        ops, length_iu, nvias = res
        router.commit(ops, nc)
        results.append((name, pcbnew.ToMM(length_iu), nvias, len(ops), time.time() - t1))
        print(f"routed {name}: length={pcbnew.ToMM(length_iu):.2f}mm vias={nvias} "
              f"segs={len(ops)} time={time.time()-t1:.1f}s")

    out = args.out or args.board
    board.Save(out)
    print(f"saved to {out}")
    print(f"total time: {time.time()-t0:.1f}s")

    fails = [r for r in results if r[1] is None]
    print(f"routed {len(results)-len(fails)}/{len(results)} nets, {len(fails)} failed")
    for r in results:
        print(" ", r)


if __name__ == "__main__":
    main()
