#!/usr/bin/env python3
"""
Autoroute a PiCoCo PCB with Freerouting: DSN export -> Freerouting -> SES
import -> dangling-via / zero-length-segment cleanup -> save.

Run with KiCad's bundled Python (needs the pcbnew module):

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \\
        tools/autoroute.py <board.kicad_pcb> [--passes N] [--out routed.kicad_pcb] [--jar path]

Freerouting (the JVM) writes under ~/Library/Application Support and
~/Library/Logs, and needs the Bash sandbox disabled to run at all; run this
script itself with dangerouslyDisableSandbox too, since it launches Freerouting
as a subprocess.

The jar is untracked (too big for git) -- default location is the repo root;
override with --jar if you keep it elsewhere.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import threading
from pathlib import Path

try:
    import pcbnew  # type: ignore
except ImportError:
    sys.exit("pcbnew module not found. Run this with KiCad's bundled python3 (see docstring).")

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JAR = PROJECT_ROOT / "freerouting-2.4.1.jar"
JAVA = "/opt/homebrew/opt/openjdk/bin/java"  # the `java` on PATH may be too old (spike: v21 fails)
TIMEOUT_S = 30 * 60


def export_dsn(board: "pcbnew.BOARD", dsn_path: Path) -> None:
    if not pcbnew.ExportSpecctraDSN(board, str(dsn_path)):
        sys.exit(f"ExportSpecctraDSN failed for {dsn_path}")


def run_freerouting(jar: Path, dsn_path: Path, ses_path: Path, passes: int) -> None:
    cmd = [JAVA, "-jar", str(jar), "-de", str(dsn_path), "-do", str(ses_path),
           "-mp", str(passes), "-mt", "1"]
    print("+", " ".join(cmd))
    log_path = ses_path.with_suffix(".log")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    timer = threading.Timer(TIMEOUT_S, proc.kill)
    timer.start()
    try:
        with open(log_path, "w") as log:
            for line in proc.stdout:  # streams as Freerouting prints; blocks on hang until killed
                print(line, end="")
                log.write(line)
        proc.wait()
    finally:
        timer.cancel()
    if proc.returncode != 0:
        if proc.returncode < 0:
            sys.exit(f"Freerouting killed after {TIMEOUT_S}s cap (signal {-proc.returncode}).")
        sys.exit(f"Freerouting exited with code {proc.returncode}")


def import_ses(board: "pcbnew.BOARD", ses_path: Path) -> None:
    if not pcbnew.ImportSpecctraSES(board, str(ses_path)):
        sys.exit(f"ImportSpecctraSES failed for {ses_path}")


def clean_dangling(board: "pcbnew.BOARD", connectivity) -> tuple[int, int, int, int, float]:
    """Remove zero-length track segments, then dangling vias (not connected,
    or connected on only one copper layer -- same test KiCad's own DRC uses
    for the via_dangling rule).

    Takes the board's CONNECTIVITY_DATA (from board.GetConnectivity(), after
    board.BuildConnectivity()) rather than fetching its own: a second
    GetConnectivity() call mid-script hit a SWIG ownership bug (returned an
    unwrapped pointer missing all methods). For the same reason this makes
    the one and only board.GetTracks() call for the whole script and returns
    everything the caller needs: a second GetTracks() call anywhere after a
    connectivity operation (RecalculateRatsnest, GetUnconnectedCount, ...)
    was observed to raise "SwigPyObject is not iterable" on the *first* call
    -- SaveBoard() on the same board object afterwards is fine, just not
    GetTracks(). Caller: use these return values for the summary, then only
    call SaveBoard(); don't call board.GetTracks() again.

    Returns (vias_removed, segments_removed, vias_remaining, segments_remaining,
    track_length_mm).
    """
    tracks = list(board.GetTracks())
    segs = [t for t in tracks if t.Type() in (pcbnew.PCB_TRACE_T, pcbnew.PCB_ARC_T)]
    vias = [t for t in tracks if t.Type() == pcbnew.PCB_VIA_T]

    zero_len = [s for s in segs if s.GetStart() == s.GetEnd()]
    for s in zero_len:
        board.Remove(s)
    segs = [s for s in segs if s not in zero_len]

    connectivity.RecalculateRatsnest()
    dangling = [v for v in vias if connectivity.TestTrackEndpointDangling(v, False)]
    for v in dangling:
        board.Remove(v)
    vias = [v for v in vias if v not in dangling]

    length_mm = sum(s.GetLength() for s in segs) / pcbnew.PCB_IU_PER_MM
    return len(dangling), len(zero_len), len(vias), len(segs), length_mm


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("board", type=Path, help="input .kicad_pcb")
    ap.add_argument("--passes", type=int, default=50, help="Freerouting -mp (default 50)")
    ap.add_argument("--out", type=Path, default=None, help="output board (default: overwrite input)")
    ap.add_argument("--jar", type=Path, default=DEFAULT_JAR, help="Freerouting jar path")
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    board_path = args.board.resolve()
    if not board_path.exists():
        sys.exit(f"board not found: {board_path}")
    if not args.jar.exists():
        sys.exit(f"Freerouting jar not found: {args.jar} (pass --jar or place it at the repo root)")
    out_path = (args.out or board_path).resolve()
    dsn_path = board_path.with_suffix(".dsn")
    ses_path = board_path.with_suffix(".ses")

    board = pcbnew.LoadBoard(str(board_path))
    export_dsn(board, dsn_path)

    run_freerouting(args.jar, dsn_path, ses_path, args.passes)

    # Reload fresh for the import, matching the spike's proven two-step load/export
    # then load/import sequence rather than reusing the export-side board object.
    board = pcbnew.LoadBoard(str(board_path))
    import_ses(board, ses_path)

    board.BuildConnectivity()
    connectivity = board.GetConnectivity()
    vias_removed, segs_removed, n_vias, n_segs, length_mm = clean_dangling(board, connectivity)

    # Unrouted count last: nothing below touches `board` again.
    connectivity.RecalculateRatsnest()
    unrouted = connectivity.GetUnconnectedCount(False)

    pcbnew.SaveBoard(str(out_path), board)

    print(f"saved: {out_path}")
    print(f"unrouted (ratsnest connections remaining): {unrouted}")
    print(f"vias: {n_vias} (dangling removed: {vias_removed})")
    print(f"track segments: {n_segs} (zero-length removed: {segs_removed})")
    print(f"total track length: {length_mm:.1f} mm")


if __name__ == "__main__":
    main()
