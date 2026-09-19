#!/usr/bin/env python3
"""Finish a Freerouting result: route what it left, pour GND, close GND, DRC.

usage: finish_route.py BOARD [--rounds N] [--rip NET ...]   (KiCad's bundled python3,
       Bash sandbox disabled: it runs kicad-cli pcb drc)

Loop (up to N rounds): DRC -> take one unconnected pair per non-GND net -> grid_route
them (shortest first). Before the first round it strips every GND track/via (the pours
carry GND) and rips the nets named with --rip (their tracks are re-routed in the loop).
Afterwards: restore the GND items that do not collide with the new copper, dedupe /
space GND vias (hole-to-hole 0.25 mm), pour.py, gnd_fix.py --drc, final DRC summary.
Exit 1 if unconnected items or DRC errors remain. Everything is in-place on BOARD; a
copy of the input is left beside it as BOARD-pre-finish.kicad_pcb. --skip-route resumes
at the GND-restore step (after a crash or a manual routing fix).
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gnd_fix  # noqa: E402
import grid_route  # noqa: E402
import pour  # noqa: E402

CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
PY = sys.executable
HERE = os.path.dirname(os.path.abspath(__file__))
mm = lambda v: v / 1e6  # noqa: E731


def drc(board_path, json_path, parity=False):
    cmd = [CLI, "pcb", "drc", "--severity-all", "--format", "json", "-o", json_path]
    if parity:
        cmd.append("--schematic-parity")
    subprocess.run(cmd + [board_path], capture_output=True)
    return json.load(open(json_path))


def pairs(d):
    seen, out = set(), []
    for x in d["unconnected_items"]:
        a, b = x["items"]
        net = re.search(r"\[(.+?)\]", a["description"]).group(1)
        if net in seen or net == "GND" or a["description"].startswith("Zone") or b["description"].startswith("Zone"):
            continue
        seen.add(net)
        out += ["--pair", net, "%.3f" % a["pos"]["x"], "%.3f" % a["pos"]["y"], "%.3f" % b["pos"]["x"], "%.3f" % b["pos"]["y"]]
    return out


def strip(board, rip_names):
    gc = board.GetNetsByName()["GND"].GetNetCode()
    rip = {board.GetNetcodeFromNetname(n) for n in rip_names}
    rm = [t for t in board.GetTracks() if t.GetNetCode() == gc or t.GetNetCode() in rip]
    for t in rm:
        board.Remove(t)
    for z in list(board.Zones()):
        if not z.GetIsRuleArea():
            board.Remove(z)
    return len(rm)


def restore_gnd(orig, new):
    gnd = new.GetNetsByName()["GND"]
    gc = gnd.GetNetCode()
    idx = grid_route.build_static_obstacles(new)
    nc = new.GetAllNetClasses()["Default"]
    clr, hc = nc.GetClearance(), new.GetDesignSettings().m_HoleClearance
    kept = dropped = 0
    holes = []  # via centres already present (any net), for the same-net hole spacing
    for t in new.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            holes.append(t.GetPosition())
    for t in orig.GetTracks():
        if t.GetNetname() != "GND":
            continue
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            if any(math.hypot(mm(p.x - h.x), mm(p.y - h.y)) < 0.9 for h in holes):
                dropped += 1
                continue
            v = pcbnew.PCB_VIA(new)
            v.SetPosition(p)
            v.SetDrill(t.GetDrill())
            v.SetWidth(t.GetWidth(pcbnew.F_Cu))
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNet(gnd)
            bad = any(idx.copper_blocked(L, v.GetEffectiveShape(L), gc, clr) for L in grid_route.LAYERS) \
                or idx.hole_blocked(v.GetEffectiveHoleShape(), gc, hc)
            if bad:
                dropped += 1
                continue
            new.Add(v)
            holes.append(p)
            for L in grid_route.LAYERS:
                idx.add_copper(L, v.GetEffectiveShape(L), gc)
            idx.add_hole(v.GetEffectiveHoleShape(), gc)
            kept += 1
        else:
            s = pcbnew.PCB_TRACK(new)
            s.SetStart(t.GetStart())
            s.SetEnd(t.GetEnd())
            s.SetWidth(t.GetWidth())
            s.SetLayer(t.GetLayer())
            s.SetNet(gnd)
            sh = s.GetEffectiveShape()
            if idx.copper_blocked(s.GetLayer(), sh, gc, clr) or idx.hole_blocked(sh, gc, hc):
                dropped += 1
                continue
            new.Add(s)
            idx.add_copper(s.GetLayer(), sh, gc)
            kept += 1
    return kept, dropped


def drop_dangling(board, d):
    """Remove the exact items DRC flagged as dangling (matched by UUID, not position:
    grid tracks share lengths and endpoints, so anything looser removes live copper)."""
    ids = {v["items"][0]["uuid"] for v in d["violations"]
           if v["type"] in ("track_dangling", "via_dangling") and "[GND]" in v["items"][0]["description"]}
    rm = [t for t in board.GetTracks() if t.m_Uuid.AsString() in ids]
    for t in rm:
        board.Remove(t)
    return len(rm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--rip", action="append", default=[])
    ap.add_argument("--skip-route", action="store_true")
    a = ap.parse_args()
    bp = a.board
    tmp = bp + ".finish.json"
    pre = bp.replace(".kicad_pcb", "-pre-finish.kicad_pcb")
    if not a.skip_route:
        shutil.copy(bp, pre)
        b = pcbnew.LoadBoard(bp)
        print("stripped GND + ripped items:", strip(b, a.rip))
        b.Save(bp)
        del b  # a second LoadBoard in this process crashes while the first board is alive (KiCad 10 SWIG)
    for r in range(0 if a.skip_route else a.rounds):
        p = pairs(drc(bp, tmp))
        print(f"round {r}: {len(p) // 6} nets to route")
        if not p:
            break
        try:
            out = subprocess.run([PY, os.path.join(HERE, "grid_route.py"), bp] + p, capture_output=True,
                                 text=True, timeout=1800).stdout
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
            print("  grid_route timed out; continuing with what it saved")
        print("\n".join(l for l in out.splitlines() if l.startswith(("routed ", "FAILED"))))
    if not a.skip_route:
        # Loading two boards after the routing subprocesses crashes inside SWIG (KiCad 10):
        # re-exec for the post-route phase in a fresh interpreter.
        sys.stdout.flush()
        os.execv(PY, [PY, os.path.abspath(__file__), bp, "--skip-route"])
    orig = pcbnew.LoadBoard(pre)
    b = pcbnew.LoadBoard(bp)
    print("GND restored/dropped:", restore_gnd(orig, b))
    pour.add_pours(b)
    b.Save(bp)
    for _ in range(2):
        d = drc(bp, tmp)
        b = pcbnew.LoadBoard(bp)
        n = drop_dangling(b, d)
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.Save(bp)
        if not n:
            break
        print("dangling GND items removed:", n)
    d = drc(bp, tmp)
    subprocess.run([PY, os.path.join(HERE, "gnd_fix.py"), bp, "--drc", tmp])
    d = drc(bp, tmp, parity=True)
    errs = [v for v in d["violations"] if v["severity"] == "error"]
    print("FINAL DRC errors:", len(errs), "unconnected:", len(d["unconnected_items"]),
          "parity:", len(d.get("schematic_parity", [])))
    for v in errs[:10]:
        print("  ERR", v["description"][:70], [(round(i["pos"]["x"], 1), round(i["pos"]["y"], 1), i["description"][:30]) for i in v["items"]])
    for x in d["unconnected_items"][:10]:
        i, j = x["items"]
        print(f"  ({i['pos']['x']:.1f},{i['pos']['y']:.1f}) {i['description'][:34]} -> ({j['pos']['x']:.1f},{j['pos']['y']:.1f}) {j['description'][:34]}")
    os.remove(tmp)
    return 1 if errs or d["unconnected_items"] else 0


if __name__ == "__main__":
    sys.exit(main())
