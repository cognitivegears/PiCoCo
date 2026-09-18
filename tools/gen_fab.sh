#!/usr/bin/env bash
# Generate PCB fabrication outputs for PiCoCo using kicad-cli.
#
# Produces Gerbers, drill files, pick-and-place, and BOM in ../fab/.
# DRC is intentionally NOT run: kicad-cli pcb drc crashes on current
# macOS with a Swift runtime error. Run DRC in the KiCad GUI before
# ordering boards.

set -euo pipefail

KICAD_CLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
if ! [ -x "$KICAD_CLI" ]; then
    if command -v kicad-cli >/dev/null 2>&1; then
        KICAD_CLI="$(command -v kicad-cli)"
    else
        echo "error: kicad-cli not found. Set KICAD_CLI env var." >&2
        exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Usage: gen_fab.sh [path/to/board.kicad_pcb] [output-dir]
PCB="${1:-$PROJECT_ROOT/PiCoCo/PiCoCo.kicad_pcb}"
SCH="${PCB%.kicad_pcb}.kicad_sch"
FAB_DIR="${2:-$PROJECT_ROOT/fab/main}"
BASE="$(basename "$PCB" .kicad_pcb)"

if [ ! -f "$PCB" ]; then
    echo "error: PCB not found: $PCB" >&2
    exit 1
fi

mkdir -p "$FAB_DIR"
# Clear stale plots so the zip only ever holds this run's layer set.
rm -f "$FAB_DIR"/*.g?? "$FAB_DIR"/*.gbr "$FAB_DIR"/*.gbrjob "$FAB_DIR"/*.drl

echo "=> Gerbers"
"$KICAD_CLI" pcb export gerbers \
    --output "$FAB_DIR/" \
    --layers "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" \
    --subtract-soldermask \
    --no-x2 \
    --use-drill-file-origin \
    "$PCB"

echo "=> Drill"
"$KICAD_CLI" pcb export drill \
    --output "$FAB_DIR/" \
    --format excellon \
    --drill-origin plot \
    --excellon-zeros-format decimal \
    --excellon-units mm \
    --generate-map \
    --map-format gerberx2 \
    "$PCB"

echo "=> Position (pick-and-place)"
"$KICAD_CLI" pcb export pos \
    --output "$FAB_DIR/$BASE-pos.csv" \
    --format csv \
    --units mm \
    --use-drill-file-origin \
    "$PCB"

echo "=> JLCPCB BOM"
"$KICAD_CLI" sch export bom --output "$FAB_DIR/$BASE-BOM-raw.csv" \
    --fields "Value,Reference,Footprint,LCSC,MPN" --labels "Value,Reference,Footprint,LCSC,MPN" \
    --group-by "Value,Footprint,LCSC" --exclude-dnp "$SCH"
python3 "$SCRIPT_DIR/jlc_post.py" bom "$FAB_DIR/$BASE-BOM-raw.csv" "$FAB_DIR/$BASE-BOM-jlc.csv"

echo "=> JLCPCB CPL (top, SMD, no DNP)"
"$KICAD_CLI" pcb export pos --output "$FAB_DIR/$BASE-pos-top.csv" --format csv --units mm \
    --side front --smd-only --exclude-dnp --use-drill-file-origin "$PCB"
python3 "$SCRIPT_DIR/jlc_post.py" cpl "$FAB_DIR/$BASE-pos-top.csv" "$FAB_DIR/$BASE-CPL-jlc.csv"

echo "=> Module stencil (paste on U1 pads only)"
rm -rf "$FAB_DIR/stencil-module" && mkdir -p "$FAB_DIR/stencil-module"
TMP_PCB="$(mktemp -t picoco).kicad_pcb"
python3 "$SCRIPT_DIR/jlc_post.py" stencil "$PCB" "$TMP_PCB"
"$KICAD_CLI" pcb export gerbers --output "$FAB_DIR/stencil-module/" --layers "F.Paste" --no-x2 --use-drill-file-origin "$TMP_PCB"
rm -f "$TMP_PCB"
# kicad-cli names the plot after the temp PCB's random basename; rename to a
# stable name so the output doesn't churn on every run.
mv "$FAB_DIR/stencil-module/"*-F_Paste.gtp "$FAB_DIR/stencil-module/$BASE-stencil-module-F_Paste.gbr"
rm -f "$FAB_DIR/stencil-module/"*-job.gbrjob

echo "=> BOM (best-effort; edit BOM.md by hand for final ordering)"
"$KICAD_CLI" sch export bom \
    --output "$FAB_DIR/$BASE-BOM.csv" \
    --preset "Grouped By Value" \
    --format-preset CSV \
    "$SCH" 2>/dev/null || \
    echo "   (skipped: 'sch export bom' not available in this kicad-cli; see BOM.md)"

echo "=> Zip for upload"
(cd "$FAB_DIR" && rm -f "$BASE-gerbers.zip" && zip -q "$BASE-gerbers.zip" *.g* *.drl)

echo
echo "=> Done. Outputs in $FAB_DIR/"
echo "   * Review $FAB_DIR/READ-BEFORE-ORDERING.txt before submitting."
echo "   * Run DRC before ordering: kicad-cli pcb drc --schematic-parity (outside the Claude sandbox) or the KiCad GUI."
